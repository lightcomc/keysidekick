#include "targeted_input.h"

#include <algorithm>

namespace keysidekick {

bool TargetedInputLedger::recordDown(const TargetedKey& key) {
    if (owns(key.usageId)) return false;

    heldKeys_.push_back(key);
    return true;
}

bool TargetedInputLedger::recordUp(int usageId, TargetedKey* released) {
    for (std::vector<TargetedKey>::iterator key = heldKeys_.begin();
         key != heldKeys_.end(); ++key) {
        if (key->usageId != usageId) continue;
        if (released) *released = *key;
        heldKeys_.erase(key);
        return true;
    }
    return false;
}

bool TargetedInputLedger::owns(int usageId) const {
    for (std::vector<TargetedKey>::const_iterator key = heldKeys_.begin();
         key != heldKeys_.end(); ++key) {
        if (key->usageId == usageId) return true;
    }
    return false;
}

std::vector<TargetedKey> TargetedInputLedger::dueRepeats(std::uint64_t nowMs) {
    std::vector<TargetedKey> due;
    for (std::vector<TargetedKey>::iterator key = heldKeys_.begin();
         key != heldKeys_.end(); ++key) {
        if (key->nextRepeatAtMs > nowMs) continue;
        due.push_back(*key);
        key->nextRepeatAtMs = nowMs + key->repeatIntervalMs;
    }
    return due;
}

bool TargetedInputLedger::nextRepeatAt(std::uint64_t* deadlineMs) const {
    if (!deadlineMs || heldKeys_.empty()) return false;

    std::uint64_t earliest = heldKeys_[0].nextRepeatAtMs;
    for (std::size_t index = 1; index < heldKeys_.size(); ++index) {
        earliest = std::min(earliest, heldKeys_[index].nextRepeatAtMs);
    }
    *deadlineMs = earliest;
    return true;
}

std::vector<TargetedKey> TargetedInputLedger::releaseAll() {
    std::vector<TargetedKey> released(heldKeys_.rbegin(), heldKeys_.rend());
    heldKeys_.clear();
    return released;
}

std::size_t TargetedInputLedger::size() const {
    return heldKeys_.size();
}

bool TargetedInputLedger::empty() const {
    return heldKeys_.empty();
}

std::uint32_t KeyboardRepeatDelayMs(unsigned int setting) {
    const unsigned int clamped = std::min(setting, 3u);
    return (clamped + 1u) * 250u;
}

std::uint32_t KeyboardRepeatIntervalMs(unsigned int setting) {
    const unsigned int clamped = std::min(setting, 31u);
    const unsigned int rateTenthsPerSecond =
        25u + (275u * clamped + 15u) / 31u;
    return (10000u + rateTenthsPerSecond / 2u) / rateTenthsPerSecond;
}

std::uint32_t TargetedMessageStateBits(TargetedMessageState state) {
    if (state == TargetedMessageState::RepeatDown) return 1u << 30;
    if (state == TargetedMessageState::KeyUp) return (1u << 30) | (1u << 31);
    return 0;
}

} // namespace keysidekick
