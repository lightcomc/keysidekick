#include "../src/supervisor.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using keysidekick::supervisor::Decision;
using keysidekick::supervisor::EventType;
using keysidekick::supervisor::Observation;
using keysidekick::supervisor::Policy;
using keysidekick::supervisor::PolicyConfig;
using keysidekick::supervisor::Status;
using keysidekick::supervisor::StopReason;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;

    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

PolicyConfig TestConfig() {
    PolicyConfig config;
    config.initialBackoffMs = 100;
    config.maxBackoffMs = 800;
    config.healthyResetMs = 1000;
    config.heartbeatTimeoutMs = 500;
    config.crashWindowMs = 5000;
    config.maxCrashesInWindow = 3;
    return config;
}

void Start(Policy& policy, std::uint64_t nowMs) {
    const char* testName = "policy start";
    const Decision decision = policy.Observe(Observation::Started(nowMs));
    REQUIRE(decision.event.type == EventType::ChildStarted);
    REQUIRE(decision.status == Status::Running);
}

void TestCrashSchedulesRestart() {
    const char* testName = "crash restart";
    Policy policy(TestConfig());
    Start(policy, 0);

    const Decision decision = policy.Observe(Observation::Exited(20, 9));

    REQUIRE(decision.event.type == EventType::RestartScheduled);
    REQUIRE(decision.status == Status::WaitingToRestart);
    REQUIRE(decision.restart);
    REQUIRE(decision.restartDelayMs == 100);
    REQUIRE(decision.event.exitCode == 9);
    REQUIRE(policy.crashCount() == 1);
}

void TestCleanExitStopsWithoutRestart() {
    const char* testName = "clean stop";
    Policy policy(TestConfig());
    Start(policy, 0);

    const Decision decision = policy.Observe(Observation::Exited(20, 0));

    REQUIRE(decision.event.type == EventType::CleanExit);
    REQUIRE(decision.status == Status::Stopped);
    REQUIRE(decision.stopReason == StopReason::CleanExit);
    REQUIRE(!decision.restart);
    REQUIRE(policy.crashCount() == 0);
}

void TestBackoffDoublesAndCaps() {
    const char* testName = "backoff doubles and caps";
    PolicyConfig config = TestConfig();
    config.maxCrashesInWindow = 20;
    Policy policy(config);

    Start(policy, 0);
    REQUIRE(policy.Observe(Observation::Exited(10, 1)).restartDelayMs == 100);
    Start(policy, 110);
    REQUIRE(policy.Observe(Observation::Exited(120, 1)).restartDelayMs == 200);
    Start(policy, 320);
    REQUIRE(policy.Observe(Observation::Exited(330, 1)).restartDelayMs == 400);
    Start(policy, 730);
    REQUIRE(policy.Observe(Observation::Exited(740, 1)).restartDelayMs == 800);
    Start(policy, 1540);
    REQUIRE(policy.Observe(Observation::Exited(1550, 1)).restartDelayMs == 800);
}

void TestCrashLoopCutoff() {
    const char* testName = "crash loop cutoff";
    Policy policy(TestConfig());

    Start(policy, 0);
    REQUIRE(policy.Observe(Observation::Exited(10, 1)).restart);
    Start(policy, 110);
    REQUIRE(policy.Observe(Observation::Exited(120, 1)).restart);
    Start(policy, 320);
    const Decision decision = policy.Observe(Observation::Exited(330, 1));

    REQUIRE(decision.event.type == EventType::CrashLoopCutoff);
    REQUIRE(decision.status == Status::Cutoff);
    REQUIRE(decision.stopReason == StopReason::CrashLoop);
    REQUIRE(!decision.restart);
    REQUIRE(policy.crashCount() == 3);
}

void TestOldCrashesFallOutOfRollingWindow() {
    const char* testName = "rolling crash window";
    PolicyConfig config = TestConfig();
    config.crashWindowMs = 1000;
    Policy policy(config);

    Start(policy, 0);
    REQUIRE(policy.Observe(Observation::Exited(10, 1)).restart);
    Start(policy, 110);
    REQUIRE(policy.Observe(Observation::Exited(120, 1)).restart);
    Start(policy, 2000);
    const Decision decision = policy.Observe(Observation::Exited(2010, 1));

    REQUIRE(decision.restart);
    REQUIRE(policy.crashCount() == 1);
}

void TestHealthyRuntimeResetsBackoffAndCrashHistory() {
    const char* testName = "healthy reset";
    Policy policy(TestConfig());

    Start(policy, 0);
    REQUIRE(policy.Observe(Observation::Exited(10, 1)).restartDelayMs == 100);
    Start(policy, 110);
    REQUIRE(policy.Observe(Observation::Exited(120, 1)).restartDelayMs == 200);
    Start(policy, 320);

    Decision decision = policy.Observe(Observation::Heartbeat(1320));
    REQUIRE(decision.event.type == EventType::Healthy);
    REQUIRE(decision.status == Status::Running);
    REQUIRE(policy.crashCount() == 0);

    decision = policy.Observe(Observation::Exited(1330, 1));
    REQUIRE(decision.restart);
    REQUIRE(decision.restartDelayMs == 100);
    REQUIRE(policy.crashCount() == 1);
}

void TestStaleHeartbeatRestartsHungChild() {
    const char* testName = "hang detection";
    Policy policy(TestConfig());
    Start(policy, 100);
    REQUIRE(policy.Observe(Observation::Heartbeat(400)).event.type == EventType::HeartbeatReceived);

    const Decision notStale = policy.Observe(Observation::Tick(899));
    REQUIRE(notStale.event.type == EventType::None);
    REQUIRE(notStale.status == Status::Running);

    const Decision stale = policy.Observe(Observation::Tick(900));
    REQUIRE(stale.event.type == EventType::HeartbeatStale);
    REQUIRE(stale.status == Status::StoppingHungChild);
    REQUIRE(stale.terminateChild);
    REQUIRE(!stale.restart);

    const Decision exited = policy.Observe(Observation::Exited(910, 1));
    REQUIRE(exited.event.type == EventType::RestartScheduled);
    REQUIRE(exited.restart);
    REQUIRE(exited.restartDelayMs == 100);
}

void TestShutdownRequestsGracefulThenBoundedForce() {
    const char* testName = "graceful shutdown then force";
    Policy policy(TestConfig());
    Start(policy, 0);

    Decision decision = policy.Observe(Observation::ShutdownRequested(10));
    REQUIRE(decision.event.type == EventType::ShutdownRequested);
    REQUIRE(decision.status == Status::ShuttingDown);
    REQUIRE(decision.requestGracefulStop);
    REQUIRE(!decision.forceTerminateChild);

    decision = policy.Observe(Observation::ForceDeadlineExpired(510));
    REQUIRE(decision.event.type == EventType::ForceTerminateRequested);
    REQUIRE(decision.status == Status::ShuttingDown);
    REQUIRE(decision.forceTerminateChild);
    REQUIRE(!decision.restart);

    decision = policy.Observe(Observation::Exited(520, 1));
    REQUIRE(decision.event.type == EventType::ShutdownComplete);
    REQUIRE(decision.status == Status::Stopped);
    REQUIRE(decision.stopReason == StopReason::Shutdown);
    REQUIRE(!decision.restart);
}

void TestStructuredStatusAndEventNames() {
    const char* testName = "structured status and events";
    Policy policy(TestConfig());
    const Decision started = policy.Observe(Observation::Started(7));

    REQUIRE(started.event.timestampMs == 7);
    REQUIRE(started.event.type == EventType::ChildStarted);
    REQUIRE(started.status == Status::Running);
    REQUIRE(keysidekick::supervisor::ToString(started.event.type) == "child_started");
    REQUIRE(keysidekick::supervisor::ToString(started.status) == "running");
    REQUIRE(keysidekick::supervisor::ToString(StopReason::CrashLoop) == "crash_loop");
}

void TestAgentCommandUsesSameExecutableWithoutShell() {
    const char* testName = "same executable agent command";
    std::vector<std::wstring> arguments;
    arguments.push_back(L"--config");
    arguments.push_back(L"C:\\Config Files\\keys.ini");

    const keysidekick::supervisor::ChildCommand command =
        keysidekick::supervisor::BuildAgentCommand(
            L"C:\\Program Files\\KeySidekick\\sidekick.exe", arguments);

    REQUIRE(command.executable == L"C:\\Program Files\\KeySidekick\\sidekick.exe");
    REQUIRE(command.arguments.size() == 3);
    REQUIRE(command.arguments[0] == L"--agent");
    REQUIRE(command.arguments[1] == L"--config");
    REQUIRE(command.arguments[2] == L"C:\\Config Files\\keys.ini");
    REQUIRE(command.commandLine ==
            L"\"C:\\Program Files\\KeySidekick\\sidekick.exe\" --agent --config \"C:\\Config Files\\keys.ini\"");
    REQUIRE(!command.useShell);
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"crash restart", TestCrashSchedulesRestart},
        {"clean stop", TestCleanExitStopsWithoutRestart},
        {"backoff doubles and caps", TestBackoffDoublesAndCaps},
        {"crash loop cutoff", TestCrashLoopCutoff},
        {"rolling crash window", TestOldCrashesFallOutOfRollingWindow},
        {"healthy reset", TestHealthyRuntimeResetsBackoffAndCrashHistory},
        {"hang detection", TestStaleHeartbeatRestartsHungChild},
        {"graceful shutdown then force", TestShutdownRequestsGracefulThenBoundedForce},
        {"structured status and events", TestStructuredStatusAndEventNames},
        {"same executable agent command", TestAgentCommandUsesSameExecutableWithoutShell},
    };

    for (std::size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        tests[index].run();
        std::cout << "PASS: " << tests[index].name << '\n';
    }

    std::cout << "All supervisor tests passed (10/10).\n";
    return 0;
}
