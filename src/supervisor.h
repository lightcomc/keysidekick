#ifndef KEYSIDEKICK_SUPERVISOR_H
#define KEYSIDEKICK_SUPERVISOR_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace keysidekick {
namespace supervisor {

enum class ObservationType {
    Started,
    Heartbeat,
    Tick,
    Exited,
    StartFailed,
    ShutdownRequested,
    ForceDeadlineExpired
};

enum class EventType {
    None,
    ChildStarted,
    HeartbeatReceived,
    Healthy,
    HeartbeatStale,
    ChildStartFailed,
    RestartScheduled,
    CrashLoopCutoff,
    CleanExit,
    ShutdownRequested,
    ForceTerminateRequested,
    ShutdownComplete,
    SupervisorError
};

enum class Status {
    Idle,
    Running,
    WaitingToRestart,
    StoppingHungChild,
    ShuttingDown,
    Stopped,
    Cutoff,
    Failed
};

enum class StopReason {
    None,
    CleanExit,
    Shutdown,
    CrashLoop,
    SupervisorError
};

struct PolicyConfig {
    std::uint64_t initialBackoffMs;
    std::uint64_t maxBackoffMs;
    std::uint64_t healthyResetMs;
    std::uint64_t heartbeatTimeoutMs;
    std::uint64_t crashWindowMs;
    std::size_t maxCrashesInWindow;

    PolicyConfig();
};

struct Observation {
    ObservationType type;
    std::uint64_t timestampMs;
    std::uint32_t exitCode;
    std::string detail;

    Observation();

    static Observation Started(std::uint64_t timestampMs);
    static Observation Heartbeat(std::uint64_t timestampMs);
    static Observation Tick(std::uint64_t timestampMs);
    static Observation Exited(std::uint64_t timestampMs, std::uint32_t exitCode);
    static Observation StartFailed(std::uint64_t timestampMs,
                                   const std::string& detail);
    static Observation ShutdownRequested(std::uint64_t timestampMs);
    static Observation ForceDeadlineExpired(std::uint64_t timestampMs);
};

struct Event {
    EventType type;
    std::uint64_t timestampMs;
    std::uint32_t exitCode;
    std::uint64_t restartDelayMs;
    std::size_t crashCount;
    std::string detail;

    Event();
};

struct Decision {
    Event event;
    Status status;
    StopReason stopReason;
    bool restart;
    std::uint64_t restartDelayMs;
    bool terminateChild;
    bool requestGracefulStop;
    bool forceTerminateChild;

    Decision();
};

class Policy {
public:
    explicit Policy(const PolicyConfig& config = PolicyConfig());

    Decision Observe(const Observation& observation);

    Status status() const;
    StopReason stopReason() const;
    std::size_t crashCount() const;
    std::uint64_t nextBackoffMs() const;

private:
    Decision BaseDecision(std::uint64_t timestampMs) const;
    Decision HandleFailure(const Observation& observation, bool startFailure);
    void PruneCrashes(std::uint64_t timestampMs);
    void ResetHealthyState();
    bool HasElapsed(std::uint64_t nowMs,
                    std::uint64_t thenMs,
                    std::uint64_t intervalMs) const;

    PolicyConfig config_;
    Status status_;
    StopReason stopReason_;
    bool childRunning_;
    bool shutdownRequested_;
    bool hungChild_;
    bool healthyResetApplied_;
    std::uint64_t childStartedMs_;
    std::uint64_t lastHeartbeatMs_;
    std::uint64_t nextBackoffMs_;
    std::deque<std::uint64_t> crashTimes_;
};

std::string ToString(EventType type);
std::string ToString(Status status);
std::string ToString(StopReason reason);

struct ChildCommand {
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::wstring commandLine;
    std::wstring workingDirectory;
    bool useShell;

    ChildCommand();
};

ChildCommand BuildAgentCommand(
    const std::wstring& executable,
    const std::vector<std::wstring>& agentArguments = std::vector<std::wstring>());

class AgentSupervisorChannel {
public:
    AgentSupervisorChannel();
    ~AgentSupervisorChannel();

    bool Open(const std::wstring& heartbeatEventName,
              const std::wstring& shutdownEventName,
              std::string* errorMessage = NULL);
    bool SignalHeartbeat();
    bool IsShutdownRequested() const;
    void Close();

private:
    AgentSupervisorChannel(const AgentSupervisorChannel&);
    AgentSupervisorChannel& operator=(const AgentSupervisorChannel&);

    void* heartbeatHandle_;
    void* shutdownHandle_;
};

enum class ChildWaitResult {
    TimedOut,
    Heartbeat,
    Exited,
    Failed
};

class WindowsChildProcess {
public:
    WindowsChildProcess();
    ~WindowsChildProcess();

    bool Start(const ChildCommand& command,
               const std::wstring& heartbeatEventName,
               const std::wstring& shutdownEventName,
               std::string* errorMessage = NULL);
    ChildWaitResult Wait(std::uint32_t timeoutMs,
                         std::uint32_t* exitCode = NULL);
    bool WaitForExit(std::uint32_t timeoutMs,
                     std::uint32_t* exitCode = NULL);
    bool IsRunning() const;
    bool RequestGracefulStop();
    bool ForceTerminate(std::uint32_t exitCode);
    std::uint32_t processId() const;
    void Close();

private:
    WindowsChildProcess(const WindowsChildProcess&);
    WindowsChildProcess& operator=(const WindowsChildProcess&);

    void* processHandle_;
    void* heartbeatHandle_;
    void* shutdownHandle_;
    std::uint32_t processId_;
};

struct SupervisorOptions {
    PolicyConfig policy;
    std::uint32_t pollIntervalMs;
    std::uint32_t gracefulShutdownMs;
    std::uint32_t forceTerminationWaitMs;
    std::uint32_t forcedExitCode;
    std::function<void(const Event&, Status)> eventSink;

    SupervisorOptions();
};

struct SupervisorRunResult {
    Status status;
    StopReason stopReason;
    std::uint32_t lastExitCode;
    std::string error;

    SupervisorRunResult();
    bool ok() const;
};

class WindowsSupervisor {
public:
    explicit WindowsSupervisor(
        const SupervisorOptions& options = SupervisorOptions());
    ~WindowsSupervisor();

    SupervisorRunResult Run(
        const std::wstring& executable,
        const std::vector<std::wstring>& agentArguments =
            std::vector<std::wstring>());
    bool RequestShutdown();

private:
    WindowsSupervisor(const WindowsSupervisor&);
    WindowsSupervisor& operator=(const WindowsSupervisor&);

    SupervisorOptions options_;
    void* shutdownRequestHandle_;
};

} // namespace supervisor
} // namespace keysidekick

#endif
