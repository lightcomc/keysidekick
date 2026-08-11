#include "runtime_state.h"

namespace keysidekick {

RuntimeSnapshot::RuntimeSnapshot()
    : lifecycle(LifecycleState::starting),
      deviceHealth(Health::unknown),
      httpHealth(Health::unknown),
      trayHealth(Health::unknown),
      configHealth(Health::unknown),
      profileMode(ProfileMode::normal),
      revision(0),
      lastErrorCode(0),
      lastReportMonotonicMs(0) {}

RuntimeState::RuntimeState()
    : resumeState_(LifecycleState::waiting_device) {}

RuntimeSnapshot RuntimeState::Snapshot() const {
    lock_guard<mutex> lock(mutex_);
    return snapshot_;
}

bool RuntimeState::TransitionTo(LifecycleState next) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || !IsAllowedTransition(snapshot_.lifecycle, next)) {
        return false;
    }

    snapshot_.lifecycle = next;
    if (next != LifecycleState::error) {
        snapshot_.lastErrorCode = 0;
        snapshot_.lastErrorMessage.clear();
    }
    CommitChangeLocked();
    return true;
}

bool RuntimeState::PauseSession() {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || snapshot_.lifecycle == LifecycleState::paused_session) {
        return false;
    }

    switch (snapshot_.lifecycle) {
        case LifecycleState::starting:
            resumeState_ = LifecycleState::waiting_device;
            break;
        case LifecycleState::waiting_device:
        case LifecycleState::opening:
        case LifecycleState::connected:
        case LifecycleState::reconnecting:
        case LifecycleState::error:
            resumeState_ = snapshot_.lifecycle;
            break;
        case LifecycleState::paused_session:
        case LifecycleState::stopping:
            return false;
    }

    snapshot_.lifecycle = LifecycleState::paused_session;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::ResumeSession() {
    lock_guard<mutex> lock(mutex_);
    if (snapshot_.lifecycle != LifecycleState::paused_session ||
        resumeState_ == LifecycleState::paused_session ||
        resumeState_ == LifecycleState::stopping) {
        return false;
    }

    snapshot_.lifecycle = resumeState_;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::SetError(int code, const std::string& message) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked()) {
        return false;
    }
    if (snapshot_.lifecycle == LifecycleState::error &&
        snapshot_.lastErrorCode == code &&
        snapshot_.lastErrorMessage == message) {
        return false;
    }

    snapshot_.lifecycle = LifecycleState::error;
    snapshot_.lastErrorCode = code;
    snapshot_.lastErrorMessage = message;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::Stop() {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked()) {
        return false;
    }

    snapshot_.lifecycle = LifecycleState::stopping;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateDeviceHealth(Health health) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || snapshot_.deviceHealth == health) {
        return false;
    }
    snapshot_.deviceHealth = health;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateHttpHealth(Health health) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || snapshot_.httpHealth == health) {
        return false;
    }
    snapshot_.httpHealth = health;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateTrayHealth(Health health) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || snapshot_.trayHealth == health) {
        return false;
    }
    snapshot_.trayHealth = health;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateConfigHealth(Health health) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || snapshot_.configHealth == health) {
        return false;
    }
    snapshot_.configHealth = health;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateSubsystemHealth(Health device,
                                         Health http,
                                         Health tray,
                                         Health config) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked()) {
        return false;
    }
    if (snapshot_.deviceHealth == device &&
        snapshot_.httpHealth == http &&
        snapshot_.trayHealth == tray &&
        snapshot_.configHealth == config) {
        return false;
    }

    snapshot_.deviceHealth = device;
    snapshot_.httpHealth = http;
    snapshot_.trayHealth = tray;
    snapshot_.configHealth = config;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::UpdateProfile(const std::string& id,
                                 const std::string& name,
                                 ProfileMode mode) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked()) {
        return false;
    }
    if (snapshot_.activeProfileId == id &&
        snapshot_.activeProfileName == name &&
        snapshot_.profileMode == mode) {
        return false;
    }

    snapshot_.activeProfileId = id;
    snapshot_.activeProfileName = name;
    snapshot_.profileMode = mode;
    CommitChangeLocked();
    return true;
}

bool RuntimeState::MarkReport(std::uint64_t monotonicMs) {
    lock_guard<mutex> lock(mutex_);
    if (IsStoppingLocked() || monotonicMs <= snapshot_.lastReportMonotonicMs) {
        return false;
    }

    snapshot_.lastReportMonotonicMs = monotonicMs;
    CommitChangeLocked();
    return true;
}

WaitResult RuntimeState::WaitForRevision(
    std::uint64_t observedRevision,
    std::chrono::milliseconds timeout) const {
    unique_lock<mutex> lock(mutex_);
    const bool ready = changed_.wait_for(lock, timeout, [this, observedRevision]() {
        return snapshot_.revision > observedRevision ||
               snapshot_.lifecycle == LifecycleState::stopping;
    });

    WaitResult result;
    result.snapshot = snapshot_;
    if (snapshot_.lifecycle == LifecycleState::stopping) {
        result.status = WaitResult::Status::shutdown;
    } else if (ready) {
        result.status = WaitResult::Status::changed;
    } else {
        result.status = WaitResult::Status::timeout;
    }
    return result;
}

bool RuntimeState::IsAllowedTransition(LifecycleState from,
                                       LifecycleState to) {
    if (from == to || to == LifecycleState::paused_session ||
        to == LifecycleState::error || to == LifecycleState::stopping) {
        return false;
    }

    switch (from) {
        case LifecycleState::starting:
            return to == LifecycleState::waiting_device ||
                   to == LifecycleState::opening;
        case LifecycleState::waiting_device:
            return to == LifecycleState::opening ||
                   to == LifecycleState::reconnecting;
        case LifecycleState::opening:
            return to == LifecycleState::connected ||
                   to == LifecycleState::waiting_device ||
                   to == LifecycleState::reconnecting;
        case LifecycleState::connected:
            return to == LifecycleState::reconnecting ||
                   to == LifecycleState::waiting_device;
        case LifecycleState::reconnecting:
            return to == LifecycleState::waiting_device ||
                   to == LifecycleState::opening ||
                   to == LifecycleState::connected;
        case LifecycleState::error:
            return to == LifecycleState::waiting_device ||
                   to == LifecycleState::opening ||
                   to == LifecycleState::reconnecting;
        case LifecycleState::paused_session:
        case LifecycleState::stopping:
            return false;
    }
    return false;
}

bool RuntimeState::IsStoppingLocked() const {
    return snapshot_.lifecycle == LifecycleState::stopping;
}

void RuntimeState::CommitChangeLocked() {
    ++snapshot_.revision;
    changed_.notify_all();
}

}  // namespace keysidekick
