#include "supervisor.h"

#include <algorithm>
#include <limits>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace keysidekick {
namespace supervisor {

namespace {

std::uint64_t SaturatingDouble(std::uint64_t value, std::uint64_t cap) {
    if (value >= cap) return cap;
    if (value > std::numeric_limits<std::uint64_t>::max() / 2) return cap;
    return std::min(value * 2, cap);
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    if (argument.empty()) return L"\"\"";

    const bool needsQuotes =
        argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) return argument;

    std::wstring quoted(1, L'\"');
    std::size_t backslashes = 0;
    for (std::wstring::const_iterator character = argument.begin();
         character != argument.end(); ++character) {
        if (*character == L'\\') {
            ++backslashes;
            continue;
        }
        if (*character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(*character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring BuildCommandLine(const std::wstring& executable,
                              const std::vector<std::wstring>& arguments) {
    std::wstring commandLine = QuoteWindowsArgument(executable);
    for (std::vector<std::wstring>::const_iterator argument = arguments.begin();
         argument != arguments.end(); ++argument) {
        commandLine.push_back(L' ');
        commandLine.append(QuoteWindowsArgument(*argument));
    }
    return commandLine;
}

#ifdef _WIN32

HANDLE AsHandle(void* handle) {
    return static_cast<HANDLE>(handle);
}

void* AsVoid(HANDLE handle) {
    return static_cast<void*>(handle);
}

std::string WindowsErrorMessage(const char* operation) {
    std::ostringstream message;
    message << operation << " failed with Windows error " << GetLastError();
    return message.str();
}

std::wstring UniqueEventName(const wchar_t* purpose) {
    static volatile LONG sequence = 0;
    std::wostringstream name;
    name << L"Local\\KeySidekick.Supervisor." << GetCurrentProcessId()
         << L'.' << InterlockedIncrement(&sequence) << L'.' << purpose;
    return name.str();
}

bool IsSignaled(HANDLE handle) {
    return handle != NULL && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

std::uint64_t MonotonicMilliseconds() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

void SleepBounded(std::uint64_t delayMs, HANDLE shutdownHandle) {
    while (delayMs != 0) {
        const DWORD slice = static_cast<DWORD>(std::min<std::uint64_t>(
            delayMs, std::numeric_limits<DWORD>::max() - 1));
        if (WaitForSingleObject(shutdownHandle, slice) == WAIT_OBJECT_0) return;
        delayMs -= slice;
    }
}

#endif

} // namespace

PolicyConfig::PolicyConfig()
    : initialBackoffMs(250),
      maxBackoffMs(30000),
      healthyResetMs(60000),
      heartbeatTimeoutMs(10000),
      crashWindowMs(60000),
      maxCrashesInWindow(5) {}

Observation::Observation()
    : type(ObservationType::Tick), timestampMs(0), exitCode(0) {}

Observation Observation::Started(std::uint64_t timestampMs) {
    Observation observation;
    observation.type = ObservationType::Started;
    observation.timestampMs = timestampMs;
    return observation;
}

Observation Observation::Heartbeat(std::uint64_t timestampMs) {
    Observation observation;
    observation.type = ObservationType::Heartbeat;
    observation.timestampMs = timestampMs;
    return observation;
}

Observation Observation::Tick(std::uint64_t timestampMs) {
    Observation observation;
    observation.type = ObservationType::Tick;
    observation.timestampMs = timestampMs;
    return observation;
}

Observation Observation::Exited(std::uint64_t timestampMs,
                                std::uint32_t exitCode) {
    Observation observation;
    observation.type = ObservationType::Exited;
    observation.timestampMs = timestampMs;
    observation.exitCode = exitCode;
    return observation;
}

Observation Observation::StartFailed(std::uint64_t timestampMs,
                                     const std::string& detail) {
    Observation observation;
    observation.type = ObservationType::StartFailed;
    observation.timestampMs = timestampMs;
    observation.detail = detail;
    return observation;
}

Observation Observation::ShutdownRequested(std::uint64_t timestampMs) {
    Observation observation;
    observation.type = ObservationType::ShutdownRequested;
    observation.timestampMs = timestampMs;
    return observation;
}

Observation Observation::ForceDeadlineExpired(std::uint64_t timestampMs) {
    Observation observation;
    observation.type = ObservationType::ForceDeadlineExpired;
    observation.timestampMs = timestampMs;
    return observation;
}

Event::Event()
    : type(EventType::None),
      timestampMs(0),
      exitCode(0),
      restartDelayMs(0),
      crashCount(0) {}

Decision::Decision()
    : status(Status::Idle),
      stopReason(StopReason::None),
      restart(false),
      restartDelayMs(0),
      terminateChild(false),
      requestGracefulStop(false),
      forceTerminateChild(false) {}

Policy::Policy(const PolicyConfig& config)
    : config_(config),
      status_(Status::Idle),
      stopReason_(StopReason::None),
      childRunning_(false),
      shutdownRequested_(false),
      hungChild_(false),
      healthyResetApplied_(false),
      childStartedMs_(0),
      lastHeartbeatMs_(0),
      nextBackoffMs_(config.initialBackoffMs) {
    if (config_.maxBackoffMs < config_.initialBackoffMs) {
        config_.maxBackoffMs = config_.initialBackoffMs;
    }
    if (config_.maxCrashesInWindow == 0) {
        config_.maxCrashesInWindow = 1;
    }
}

Decision Policy::BaseDecision(std::uint64_t timestampMs) const {
    Decision decision;
    decision.status = status_;
    decision.stopReason = stopReason_;
    decision.event.timestampMs = timestampMs;
    decision.event.crashCount = crashTimes_.size();
    return decision;
}

bool Policy::HasElapsed(std::uint64_t nowMs,
                        std::uint64_t thenMs,
                        std::uint64_t intervalMs) const {
    return nowMs >= thenMs && nowMs - thenMs >= intervalMs;
}

void Policy::PruneCrashes(std::uint64_t timestampMs) {
    while (!crashTimes_.empty() &&
           HasElapsed(timestampMs, crashTimes_.front(), config_.crashWindowMs) &&
           timestampMs - crashTimes_.front() > config_.crashWindowMs) {
        crashTimes_.pop_front();
    }
}

void Policy::ResetHealthyState() {
    crashTimes_.clear();
    nextBackoffMs_ = config_.initialBackoffMs;
    healthyResetApplied_ = true;
}

Decision Policy::HandleFailure(const Observation& observation,
                               bool startFailure) {
    childRunning_ = false;
    PruneCrashes(observation.timestampMs);
    crashTimes_.push_back(observation.timestampMs);

    Decision decision = BaseDecision(observation.timestampMs);
    decision.event.exitCode = observation.exitCode;
    decision.event.detail = observation.detail;
    decision.event.crashCount = crashTimes_.size();

    if (crashTimes_.size() >= config_.maxCrashesInWindow) {
        status_ = Status::Cutoff;
        stopReason_ = StopReason::CrashLoop;
        decision.status = status_;
        decision.stopReason = stopReason_;
        decision.event.type = EventType::CrashLoopCutoff;
        return decision;
    }

    status_ = Status::WaitingToRestart;
    decision.status = status_;
    decision.event.type = EventType::RestartScheduled;
    if (startFailure && decision.event.detail.empty()) {
        decision.event.detail = "child start failed";
    }
    decision.restart = true;
    decision.restartDelayMs = nextBackoffMs_;
    decision.event.restartDelayMs = decision.restartDelayMs;
    nextBackoffMs_ = SaturatingDouble(nextBackoffMs_, config_.maxBackoffMs);
    return decision;
}

Decision Policy::Observe(const Observation& observation) {
    Decision decision = BaseDecision(observation.timestampMs);

    switch (observation.type) {
        case ObservationType::Started:
            childRunning_ = true;
            hungChild_ = false;
            healthyResetApplied_ = false;
            childStartedMs_ = observation.timestampMs;
            lastHeartbeatMs_ = observation.timestampMs;
            status_ = shutdownRequested_ ? Status::ShuttingDown : Status::Running;
            stopReason_ = StopReason::None;
            decision.status = status_;
            decision.stopReason = stopReason_;
            decision.event.type = EventType::ChildStarted;
            break;

        case ObservationType::Heartbeat:
            if (!childRunning_ || shutdownRequested_) break;
            lastHeartbeatMs_ = observation.timestampMs;
            decision.event.type = EventType::HeartbeatReceived;
            if (!healthyResetApplied_ &&
                HasElapsed(observation.timestampMs, childStartedMs_,
                           config_.healthyResetMs)) {
                ResetHealthyState();
                decision.event.type = EventType::Healthy;
                decision.event.crashCount = 0;
            }
            break;

        case ObservationType::Tick:
            if (!childRunning_ || shutdownRequested_ || hungChild_) break;
            if (!healthyResetApplied_ &&
                HasElapsed(observation.timestampMs, childStartedMs_,
                           config_.healthyResetMs)) {
                ResetHealthyState();
                decision.event.type = EventType::Healthy;
                decision.event.crashCount = 0;
            }
            if (HasElapsed(observation.timestampMs, lastHeartbeatMs_,
                           config_.heartbeatTimeoutMs)) {
                hungChild_ = true;
                status_ = Status::StoppingHungChild;
                decision.status = status_;
                decision.event.type = EventType::HeartbeatStale;
                decision.terminateChild = true;
            }
            break;

        case ObservationType::Exited:
            childRunning_ = false;
            if (shutdownRequested_) {
                status_ = Status::Stopped;
                stopReason_ = StopReason::Shutdown;
                decision.status = status_;
                decision.stopReason = stopReason_;
                decision.event.type = EventType::ShutdownComplete;
                decision.event.exitCode = observation.exitCode;
                break;
            }
            if (observation.exitCode == 0 && !hungChild_) {
                status_ = Status::Stopped;
                stopReason_ = StopReason::CleanExit;
                decision.status = status_;
                decision.stopReason = stopReason_;
                decision.event.type = EventType::CleanExit;
                break;
            }
            return HandleFailure(observation, false);

        case ObservationType::StartFailed:
            return HandleFailure(observation, true);

        case ObservationType::ShutdownRequested:
            shutdownRequested_ = true;
            status_ = childRunning_ ? Status::ShuttingDown : Status::Stopped;
            stopReason_ = childRunning_ ? StopReason::None : StopReason::Shutdown;
            decision.status = status_;
            decision.stopReason = stopReason_;
            decision.event.type = childRunning_
                ? EventType::ShutdownRequested
                : EventType::ShutdownComplete;
            decision.requestGracefulStop = childRunning_;
            break;

        case ObservationType::ForceDeadlineExpired:
            if (shutdownRequested_ && childRunning_) {
                status_ = Status::ShuttingDown;
                decision.status = status_;
                decision.event.type = EventType::ForceTerminateRequested;
                decision.forceTerminateChild = true;
            }
            break;
    }

    return decision;
}

Status Policy::status() const {
    return status_;
}

StopReason Policy::stopReason() const {
    return stopReason_;
}

std::size_t Policy::crashCount() const {
    return crashTimes_.size();
}

std::uint64_t Policy::nextBackoffMs() const {
    return nextBackoffMs_;
}

std::string ToString(EventType type) {
    switch (type) {
        case EventType::None: return "none";
        case EventType::ChildStarted: return "child_started";
        case EventType::HeartbeatReceived: return "heartbeat_received";
        case EventType::Healthy: return "healthy";
        case EventType::HeartbeatStale: return "heartbeat_stale";
        case EventType::ChildStartFailed: return "child_start_failed";
        case EventType::RestartScheduled: return "restart_scheduled";
        case EventType::CrashLoopCutoff: return "crash_loop_cutoff";
        case EventType::CleanExit: return "clean_exit";
        case EventType::ShutdownRequested: return "shutdown_requested";
        case EventType::ForceTerminateRequested: return "force_terminate_requested";
        case EventType::ShutdownComplete: return "shutdown_complete";
        case EventType::SupervisorError: return "supervisor_error";
    }
    return "unknown";
}

std::string ToString(Status status) {
    switch (status) {
        case Status::Idle: return "idle";
        case Status::Running: return "running";
        case Status::WaitingToRestart: return "waiting_to_restart";
        case Status::StoppingHungChild: return "stopping_hung_child";
        case Status::ShuttingDown: return "shutting_down";
        case Status::Stopped: return "stopped";
        case Status::Cutoff: return "cutoff";
        case Status::Failed: return "failed";
    }
    return "unknown";
}

std::string ToString(StopReason reason) {
    switch (reason) {
        case StopReason::None: return "none";
        case StopReason::CleanExit: return "clean_exit";
        case StopReason::Shutdown: return "shutdown";
        case StopReason::CrashLoop: return "crash_loop";
        case StopReason::SupervisorError: return "supervisor_error";
    }
    return "unknown";
}

ChildCommand::ChildCommand() : useShell(false) {}

ChildCommand BuildAgentCommand(
    const std::wstring& executable,
    const std::vector<std::wstring>& agentArguments) {
    ChildCommand command;
    command.executable = executable;
    command.arguments.push_back(L"--agent");
    command.arguments.insert(command.arguments.end(), agentArguments.begin(),
                             agentArguments.end());
    command.commandLine = BuildCommandLine(command.executable, command.arguments);
    return command;
}

AgentSupervisorChannel::AgentSupervisorChannel()
    : heartbeatHandle_(NULL), shutdownHandle_(NULL) {}

AgentSupervisorChannel::~AgentSupervisorChannel() {
    Close();
}

bool AgentSupervisorChannel::Open(const std::wstring& heartbeatEventName,
                                  const std::wstring& shutdownEventName,
                                  std::string* errorMessage) {
    Close();
#ifdef _WIN32
    HANDLE heartbeat = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                  heartbeatEventName.c_str());
    if (heartbeat == NULL) {
        if (errorMessage != NULL) {
            *errorMessage = WindowsErrorMessage("OpenEventW(heartbeat)");
        }
        return false;
    }

    HANDLE shutdown = OpenEventW(SYNCHRONIZE, FALSE, shutdownEventName.c_str());
    if (shutdown == NULL) {
        if (errorMessage != NULL) {
            *errorMessage = WindowsErrorMessage("OpenEventW(shutdown)");
        }
        CloseHandle(heartbeat);
        return false;
    }

    heartbeatHandle_ = AsVoid(heartbeat);
    shutdownHandle_ = AsVoid(shutdown);
    return true;
#else
    (void)heartbeatEventName;
    (void)shutdownEventName;
    if (errorMessage != NULL) *errorMessage = "Windows supervisor unavailable";
    return false;
#endif
}

bool AgentSupervisorChannel::SignalHeartbeat() {
#ifdef _WIN32
    return heartbeatHandle_ != NULL && SetEvent(AsHandle(heartbeatHandle_)) != FALSE;
#else
    return false;
#endif
}

bool AgentSupervisorChannel::IsShutdownRequested() const {
#ifdef _WIN32
    return shutdownHandle_ != NULL && IsSignaled(AsHandle(shutdownHandle_));
#else
    return false;
#endif
}

void AgentSupervisorChannel::Close() {
#ifdef _WIN32
    if (heartbeatHandle_ != NULL) CloseHandle(AsHandle(heartbeatHandle_));
    if (shutdownHandle_ != NULL) CloseHandle(AsHandle(shutdownHandle_));
#endif
    heartbeatHandle_ = NULL;
    shutdownHandle_ = NULL;
}

WindowsChildProcess::WindowsChildProcess()
    : processHandle_(NULL),
      heartbeatHandle_(NULL),
      shutdownHandle_(NULL),
      processId_(0) {}

WindowsChildProcess::~WindowsChildProcess() {
    Close();
}

bool WindowsChildProcess::Start(const ChildCommand& command,
                                const std::wstring& heartbeatEventName,
                                const std::wstring& shutdownEventName,
                                std::string* errorMessage) {
    Close();
#ifdef _WIN32
    HANDLE heartbeat = CreateEventW(NULL, FALSE, FALSE,
                                    heartbeatEventName.c_str());
    if (heartbeat == NULL) {
        if (errorMessage != NULL) {
            *errorMessage = WindowsErrorMessage("CreateEventW(heartbeat)");
        }
        return false;
    }

    HANDLE shutdown = CreateEventW(NULL, TRUE, FALSE, shutdownEventName.c_str());
    if (shutdown == NULL) {
        if (errorMessage != NULL) {
            *errorMessage = WindowsErrorMessage("CreateEventW(shutdown)");
        }
        CloseHandle(heartbeat);
        return false;
    }

    std::vector<wchar_t> mutableCommandLine(command.commandLine.begin(),
                                             command.commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    const wchar_t* workingDirectory = command.workingDirectory.empty()
        ? NULL
        : command.workingDirectory.c_str();
    const BOOL started = CreateProcessW(
        command.executable.c_str(), mutableCommandLine.data(), NULL, NULL, FALSE,
        CREATE_NEW_PROCESS_GROUP, NULL, workingDirectory, &startupInfo,
        &processInfo);
    if (!started) {
        if (errorMessage != NULL) {
            *errorMessage = WindowsErrorMessage("CreateProcessW");
        }
        CloseHandle(heartbeat);
        CloseHandle(shutdown);
        return false;
    }

    CloseHandle(processInfo.hThread);
    processHandle_ = AsVoid(processInfo.hProcess);
    heartbeatHandle_ = AsVoid(heartbeat);
    shutdownHandle_ = AsVoid(shutdown);
    processId_ = processInfo.dwProcessId;
    return true;
#else
    (void)command;
    (void)heartbeatEventName;
    (void)shutdownEventName;
    if (errorMessage != NULL) *errorMessage = "Windows supervisor unavailable";
    return false;
#endif
}

ChildWaitResult WindowsChildProcess::Wait(std::uint32_t timeoutMs,
                                          std::uint32_t* exitCode) {
#ifdef _WIN32
    if (processHandle_ == NULL || heartbeatHandle_ == NULL) {
        return ChildWaitResult::Failed;
    }
    HANDLE handles[] = {AsHandle(processHandle_), AsHandle(heartbeatHandle_)};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
    if (result == WAIT_TIMEOUT) return ChildWaitResult::TimedOut;
    if (result == WAIT_OBJECT_0 + 1) return ChildWaitResult::Heartbeat;
    if (result == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (!GetExitCodeProcess(AsHandle(processHandle_), &code)) {
            return ChildWaitResult::Failed;
        }
        if (exitCode != NULL) *exitCode = code;
        return ChildWaitResult::Exited;
    }
    return ChildWaitResult::Failed;
#else
    (void)timeoutMs;
    (void)exitCode;
    return ChildWaitResult::Failed;
#endif
}

bool WindowsChildProcess::WaitForExit(std::uint32_t timeoutMs,
                                      std::uint32_t* exitCode) {
#ifdef _WIN32
    if (processHandle_ == NULL) return true;
    if (WaitForSingleObject(AsHandle(processHandle_), timeoutMs) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(AsHandle(processHandle_), &code)) return false;
    if (exitCode != NULL) *exitCode = code;
    return true;
#else
    (void)timeoutMs;
    (void)exitCode;
    return false;
#endif
}

bool WindowsChildProcess::IsRunning() const {
#ifdef _WIN32
    if (processHandle_ == NULL) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(AsHandle(processHandle_), &exitCode) != FALSE &&
           exitCode == STILL_ACTIVE;
#else
    return false;
#endif
}

bool WindowsChildProcess::RequestGracefulStop() {
#ifdef _WIN32
    return shutdownHandle_ != NULL && SetEvent(AsHandle(shutdownHandle_)) != FALSE;
#else
    return false;
#endif
}

bool WindowsChildProcess::ForceTerminate(std::uint32_t exitCode) {
#ifdef _WIN32
    return processHandle_ != NULL &&
           TerminateProcess(AsHandle(processHandle_), exitCode) != FALSE;
#else
    (void)exitCode;
    return false;
#endif
}

std::uint32_t WindowsChildProcess::processId() const {
    return processId_;
}

void WindowsChildProcess::Close() {
#ifdef _WIN32
    if (processHandle_ != NULL) CloseHandle(AsHandle(processHandle_));
    if (heartbeatHandle_ != NULL) CloseHandle(AsHandle(heartbeatHandle_));
    if (shutdownHandle_ != NULL) CloseHandle(AsHandle(shutdownHandle_));
#endif
    processHandle_ = NULL;
    heartbeatHandle_ = NULL;
    shutdownHandle_ = NULL;
    processId_ = 0;
}

SupervisorOptions::SupervisorOptions()
    : pollIntervalMs(250),
      gracefulShutdownMs(3000),
      forceTerminationWaitMs(1000),
      forcedExitCode(0xE0000001u) {}

SupervisorRunResult::SupervisorRunResult()
    : status(Status::Idle),
      stopReason(StopReason::None),
      lastExitCode(0) {}

bool SupervisorRunResult::ok() const {
    return status == Status::Stopped &&
           (stopReason == StopReason::CleanExit ||
            stopReason == StopReason::Shutdown);
}

WindowsSupervisor::WindowsSupervisor(const SupervisorOptions& options)
    : options_(options), shutdownRequestHandle_(NULL) {
#ifdef _WIN32
    shutdownRequestHandle_ = AsVoid(CreateEventW(NULL, TRUE, FALSE, NULL));
#endif
}

WindowsSupervisor::~WindowsSupervisor() {
#ifdef _WIN32
    if (shutdownRequestHandle_ != NULL) {
        CloseHandle(AsHandle(shutdownRequestHandle_));
    }
#endif
}

bool WindowsSupervisor::RequestShutdown() {
#ifdef _WIN32
    return shutdownRequestHandle_ != NULL &&
           SetEvent(AsHandle(shutdownRequestHandle_)) != FALSE;
#else
    return false;
#endif
}

SupervisorRunResult WindowsSupervisor::Run(
    const std::wstring& executable,
    const std::vector<std::wstring>& agentArguments) {
    SupervisorRunResult result;
#ifdef _WIN32
    if (executable.empty() || shutdownRequestHandle_ == NULL) {
        result.status = Status::Failed;
        result.stopReason = StopReason::SupervisorError;
        result.error = executable.empty()
            ? "child executable is empty"
            : "supervisor shutdown event is unavailable";
        return result;
    }

    ResetEvent(AsHandle(shutdownRequestHandle_));
    const ChildCommand command = BuildAgentCommand(executable, agentArguments);
    const std::wstring heartbeatName = UniqueEventName(L"heartbeat");
    const std::wstring childShutdownName = UniqueEventName(L"shutdown");
    Policy policy(options_.policy);
    WindowsChildProcess child;

    const std::function<void(const Decision&)> emit =
        [this](const Decision& decision) {
            if (decision.event.type != EventType::None && options_.eventSink) {
                options_.eventSink(decision.event, decision.status);
            }
        };

    for (;;) {
        if (IsSignaled(AsHandle(shutdownRequestHandle_))) {
            const Decision shutdown = policy.Observe(
                Observation::ShutdownRequested(MonotonicMilliseconds()));
            emit(shutdown);
            if (!child.IsRunning()) {
                result.status = shutdown.status;
                result.stopReason = shutdown.stopReason;
                return result;
            }
            child.RequestGracefulStop();
            if (!child.WaitForExit(options_.gracefulShutdownMs,
                                   &result.lastExitCode)) {
                const Decision force = policy.Observe(
                    Observation::ForceDeadlineExpired(MonotonicMilliseconds()));
                emit(force);
                if (!child.ForceTerminate(options_.forcedExitCode) ||
                    !child.WaitForExit(options_.forceTerminationWaitMs,
                                       &result.lastExitCode)) {
                    result.status = Status::Failed;
                    result.stopReason = StopReason::SupervisorError;
                    result.error = "child did not terminate within shutdown bounds";
                    return result;
                }
            }
            const Decision complete = policy.Observe(
                Observation::Exited(MonotonicMilliseconds(), result.lastExitCode));
            emit(complete);
            result.status = complete.status;
            result.stopReason = complete.stopReason;
            return result;
        }

        std::string startError;
        if (!child.Start(command, heartbeatName, childShutdownName, &startError)) {
            const Decision failure = policy.Observe(
                Observation::StartFailed(MonotonicMilliseconds(), startError));
            emit(failure);
            if (!failure.restart) {
                result.status = failure.status;
                result.stopReason = failure.stopReason;
                result.error = startError;
                return result;
            }
            SleepBounded(failure.restartDelayMs,
                         AsHandle(shutdownRequestHandle_));
            continue;
        }

        emit(policy.Observe(Observation::Started(MonotonicMilliseconds())));
        bool restart = false;
        std::uint64_t restartDelayMs = 0;
        while (child.IsRunning()) {
            if (IsSignaled(AsHandle(shutdownRequestHandle_))) break;

            const ChildWaitResult waitResult = child.Wait(options_.pollIntervalMs,
                                                           &result.lastExitCode);
            Decision decision;
            if (waitResult == ChildWaitResult::Heartbeat) {
                decision = policy.Observe(
                    Observation::Heartbeat(MonotonicMilliseconds()));
            } else if (waitResult == ChildWaitResult::Exited) {
                decision = policy.Observe(Observation::Exited(
                    MonotonicMilliseconds(), result.lastExitCode));
            } else if (waitResult == ChildWaitResult::TimedOut) {
                decision = policy.Observe(
                    Observation::Tick(MonotonicMilliseconds()));
            } else {
                result.status = Status::Failed;
                result.stopReason = StopReason::SupervisorError;
                result.error = "failed while waiting for child process";
                return result;
            }
            emit(decision);

            if (decision.terminateChild) {
                child.RequestGracefulStop();
                if (!child.WaitForExit(options_.gracefulShutdownMs,
                                       &result.lastExitCode)) {
                    if (!child.ForceTerminate(options_.forcedExitCode) ||
                        !child.WaitForExit(options_.forceTerminationWaitMs,
                                           &result.lastExitCode)) {
                        result.status = Status::Failed;
                        result.stopReason = StopReason::SupervisorError;
                        result.error = "hung child could not be terminated";
                        return result;
                    }
                }
                decision = policy.Observe(Observation::Exited(
                    MonotonicMilliseconds(), result.lastExitCode));
                emit(decision);
            }

            if (decision.restart) {
                restart = true;
                restartDelayMs = decision.restartDelayMs;
                break;
            }
            if (decision.status == Status::Stopped ||
                decision.status == Status::Cutoff) {
                result.status = decision.status;
                result.stopReason = decision.stopReason;
                return result;
            }
        }

        if (IsSignaled(AsHandle(shutdownRequestHandle_))) continue;
        if (!restart && !child.IsRunning()) {
            const Decision exited = policy.Observe(Observation::Exited(
                MonotonicMilliseconds(), result.lastExitCode));
            emit(exited);
            if (!exited.restart) {
                result.status = exited.status;
                result.stopReason = exited.stopReason;
                return result;
            }
            restart = true;
            restartDelayMs = exited.restartDelayMs;
        }

        child.Close();
        if (restart) {
            SleepBounded(restartDelayMs, AsHandle(shutdownRequestHandle_));
        }
    }
#else
    (void)executable;
    (void)agentArguments;
    result.status = Status::Failed;
    result.stopReason = StopReason::SupervisorError;
    result.error = "Windows supervisor unavailable";
    return result;
#endif
}

} // namespace supervisor
} // namespace keysidekick
