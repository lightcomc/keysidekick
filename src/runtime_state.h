#ifndef KEYSIDEKICK_RUNTIME_STATE_H
#define KEYSIDEKICK_RUNTIME_STATE_H

#include <chrono>
#include <cstdint>
#include <string>
#include "mingw_threading.h"
using std::mutex;
using std::lock_guard;
using std::unique_lock;
using std::condition_variable;

namespace keysidekick {

enum class LifecycleState {
    starting,
    waiting_device,
    opening,
    connected,
    reconnecting,
    paused_session,
    error,
    stopping
};

enum class Health {
    unknown,
    healthy,
    degraded,
    unavailable
};

enum class ProfileMode {
    normal,
    app_control,
    multi_app
};

struct RuntimeSnapshot {
    LifecycleState lifecycle;
    Health deviceHealth;
    Health httpHealth;
    Health trayHealth;
    Health configHealth;
    std::string activeProfileId;
    std::string activeProfileName;
    ProfileMode profileMode;
    std::uint64_t revision;
    int lastErrorCode;
    std::string lastErrorMessage;
    std::uint64_t lastReportMonotonicMs;

    RuntimeSnapshot();
};

struct WaitResult {
    enum class Status {
        changed,
        timeout,
        shutdown
    };

    Status status;
    RuntimeSnapshot snapshot;
};

class RuntimeState {
public:
    RuntimeState();

    RuntimeSnapshot Snapshot() const;

    bool TransitionTo(LifecycleState next);
    bool PauseSession();
    bool ResumeSession();
    bool SetError(int code, const std::string& message);
    bool Stop();

    bool UpdateDeviceHealth(Health health);
    bool UpdateHttpHealth(Health health);
    bool UpdateTrayHealth(Health health);
    bool UpdateConfigHealth(Health health);
    bool UpdateSubsystemHealth(Health device,
                               Health http,
                               Health tray,
                               Health config);
    bool UpdateProfile(const std::string& id,
                       const std::string& name,
                       ProfileMode mode);
    bool MarkReport(std::uint64_t monotonicMs);

    WaitResult WaitForRevision(
        std::uint64_t observedRevision,
        std::chrono::milliseconds timeout) const;

private:
    RuntimeState(const RuntimeState&);
    RuntimeState& operator=(const RuntimeState&);

    static bool IsAllowedTransition(LifecycleState from, LifecycleState to);
    bool IsStoppingLocked() const;
    void CommitChangeLocked();

    mutable mutex mutex_;
    mutable condition_variable changed_;
    RuntimeSnapshot snapshot_;
    LifecycleState resumeState_;
};

}  // namespace keysidekick

#endif
