#include "../src/mingw_threading.h"
#include "../src/runtime_state.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

using keysidekick::Health;
using keysidekick::LifecycleState;
using keysidekick::ProfileMode;
using keysidekick::RuntimeSnapshot;
using keysidekick::RuntimeState;
using keysidekick::WaitResult;

void TestStartupConnectsWhenDeviceAppears() {
    RuntimeState state;
    RuntimeSnapshot snapshot = state.Snapshot();

    CHECK(snapshot.lifecycle == LifecycleState::starting);
    CHECK(snapshot.revision == 0);
    CHECK(snapshot.deviceHealth == Health::unknown);
    CHECK(snapshot.lastReportMonotonicMs == 0);

    CHECK(state.UpdateSubsystemHealth(Health::unavailable,
                                      Health::healthy,
                                      Health::healthy,
                                      Health::healthy));
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    snapshot = state.Snapshot();
    CHECK(snapshot.lifecycle == LifecycleState::waiting_device);
    CHECK(snapshot.deviceHealth == Health::unavailable);
    CHECK(snapshot.lastErrorCode == 0);
    CHECK(snapshot.lastErrorMessage.empty());

    CHECK(state.UpdateDeviceHealth(Health::healthy));
    CHECK(state.TransitionTo(LifecycleState::opening));
    CHECK(state.TransitionTo(LifecycleState::connected));

    snapshot = state.Snapshot();
    CHECK(snapshot.lifecycle == LifecycleState::connected);
    CHECK(snapshot.deviceHealth == Health::healthy);
    CHECK(snapshot.httpHealth == Health::healthy);
    CHECK(snapshot.trayHealth == Health::healthy);
    CHECK(snapshot.configHealth == Health::healthy);
    CHECK(snapshot.lastErrorCode == 0);
    CHECK(snapshot.lastErrorMessage.empty());
}

void TestUnplugAndReconnect() {
    RuntimeState state;
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    CHECK(state.TransitionTo(LifecycleState::opening));
    CHECK(state.TransitionTo(LifecycleState::connected));
    CHECK(state.UpdateSubsystemHealth(Health::unavailable,
                                      Health::healthy,
                                      Health::healthy,
                                      Health::healthy));
    CHECK(state.TransitionTo(LifecycleState::reconnecting));
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    CHECK(state.TransitionTo(LifecycleState::opening));
    CHECK(state.UpdateDeviceHealth(Health::healthy));
    CHECK(state.TransitionTo(LifecycleState::connected));

    const RuntimeSnapshot snapshot = state.Snapshot();
    CHECK(snapshot.lifecycle == LifecycleState::connected);
    CHECK(snapshot.deviceHealth == Health::healthy);
}

void TestPauseResumeRestoresPreviousState() {
    RuntimeState state;
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    CHECK(state.PauseSession());
    CHECK(state.Snapshot().lifecycle == LifecycleState::paused_session);
    CHECK(state.ResumeSession());
    CHECK(state.Snapshot().lifecycle == LifecycleState::waiting_device);

    CHECK(state.TransitionTo(LifecycleState::opening));
    CHECK(state.TransitionTo(LifecycleState::connected));
    CHECK(state.PauseSession());
    CHECK(state.ResumeSession());
    CHECK(state.Snapshot().lifecycle == LifecycleState::connected);

    RuntimeState starting;
    CHECK(starting.PauseSession());
    CHECK(starting.ResumeSession());
    CHECK(starting.Snapshot().lifecycle == LifecycleState::waiting_device);
}

void TestErrorRecovery() {
    RuntimeState state;
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    CHECK(state.SetError(17, "open failed"));

    RuntimeSnapshot snapshot = state.Snapshot();
    CHECK(snapshot.lifecycle == LifecycleState::error);
    CHECK(snapshot.lastErrorCode == 17);
    CHECK(snapshot.lastErrorMessage == "open failed");

    CHECK(state.TransitionTo(LifecycleState::reconnecting));
    snapshot = state.Snapshot();
    CHECK(snapshot.lastErrorCode == 0);
    CHECK(snapshot.lastErrorMessage.empty());
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
}

void TestInvalidTransitionsAreRejected() {
    RuntimeState state;
    const std::uint64_t initialRevision = state.Snapshot().revision;

    CHECK(!state.TransitionTo(LifecycleState::connected));
    CHECK(!state.ResumeSession());
    CHECK(state.Snapshot().lifecycle == LifecycleState::starting);
    CHECK(state.Snapshot().revision == initialRevision);

    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    const std::uint64_t waitingRevision = state.Snapshot().revision;
    CHECK(!state.TransitionTo(LifecycleState::waiting_device));
    CHECK(!state.TransitionTo(LifecycleState::connected));
    CHECK(state.Snapshot().revision == waitingRevision);
}

void TestRevisionAndWaiter() {
    RuntimeState state;
    RuntimeSnapshot snapshot = state.Snapshot();
    const std::uint64_t initialRevision = snapshot.revision;

    CHECK(state.UpdateProfile("basic", "Basic", ProfileMode::normal));
    snapshot = state.Snapshot();
    CHECK(snapshot.revision == initialRevision + 1);
    CHECK(snapshot.activeProfileId == "basic");
    CHECK(snapshot.activeProfileName == "Basic");
    CHECK(snapshot.profileMode == ProfileMode::normal);

    CHECK(!state.UpdateProfile("basic", "Basic", ProfileMode::normal));
    CHECK(state.Snapshot().revision == snapshot.revision);

    CHECK(state.MarkReport(100));
    const std::uint64_t reportRevision = state.Snapshot().revision;
    CHECK(!state.MarkReport(90));
    CHECK(state.Snapshot().revision == reportRevision);
    CHECK(state.MarkReport(101));

    const std::uint64_t observedRevision = state.Snapshot().revision;
    WaitResult result;
    std::thread waiter([&state, observedRevision, &result]() {
        result = state.WaitForRevision(observedRevision, std::chrono::milliseconds(1000));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(state.UpdateHttpHealth(Health::degraded));
    waiter.join();

    CHECK(result.status == WaitResult::Status::changed);
    CHECK(result.snapshot.revision > observedRevision);
    CHECK(result.snapshot.httpHealth == Health::degraded);

    const std::uint64_t currentRevision = state.Snapshot().revision;
    result = state.WaitForRevision(currentRevision, std::chrono::milliseconds(10));
    CHECK(result.status == WaitResult::Status::timeout);
    CHECK(result.snapshot.revision == currentRevision);
}

void TestConcurrentUpdatesAreSafe() {
    RuntimeState state;
    const int updatesPerThread = 100;

    std::thread profileThread([&state, updatesPerThread]() {
        for (int index = 0; index < updatesPerThread; ++index) {
            state.UpdateProfile("profile-" + std::to_string(index),
                                "Profile " + std::to_string(index),
                                index % 2 == 0 ? ProfileMode::app_control
                                               : ProfileMode::multi_app);
        }
    });
    std::thread reportThread([&state, updatesPerThread]() {
        for (int index = 1; index <= updatesPerThread; ++index) {
            state.MarkReport(static_cast<std::uint64_t>(index));
        }
    });
    profileThread.join();
    reportThread.join();

    const RuntimeSnapshot snapshot = state.Snapshot();
    CHECK(snapshot.activeProfileId == "profile-99");
    CHECK(snapshot.profileMode == ProfileMode::multi_app);
    CHECK(snapshot.lastReportMonotonicMs == 100);
    CHECK(snapshot.revision == 200);
}

void TestStoppingWakesWaitersAndIsTerminal() {
    RuntimeState state;
    CHECK(state.TransitionTo(LifecycleState::waiting_device));
    const std::uint64_t observedRevision = state.Snapshot().revision;

    WaitResult result;
    std::thread waiter([&state, observedRevision, &result]() {
        result = state.WaitForRevision(observedRevision, std::chrono::seconds(5));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(state.Stop());
    waiter.join();

    CHECK(result.status == WaitResult::Status::shutdown);
    CHECK(result.snapshot.lifecycle == LifecycleState::stopping);
    const std::uint64_t stoppedRevision = result.snapshot.revision;
    CHECK(!state.Stop());
    CHECK(!state.TransitionTo(LifecycleState::starting));
    CHECK(!state.UpdateDeviceHealth(Health::healthy));
    CHECK(state.Snapshot().revision == stoppedRevision);

    result = state.WaitForRevision(0, std::chrono::milliseconds(10));
    CHECK(result.status == WaitResult::Status::shutdown);
}

}  // namespace

int main() {
    TestStartupConnectsWhenDeviceAppears();
    TestUnplugAndReconnect();
    TestPauseResumeRestoresPreviousState();
    TestErrorRecovery();
    TestInvalidTransitionsAreRejected();
    TestRevisionAndWaiter();
    TestConcurrentUpdatesAreSafe();
    TestStoppingWakesWaitersAndIsTerminal();

    if (failures != 0) {
        std::cerr << failures << " runtime state test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "runtime state tests passed\n";
    return EXIT_SUCCESS;
}
