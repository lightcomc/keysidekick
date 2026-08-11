#ifndef KEYSIDEKICK_TARGETED_INPUT_H
#define KEYSIDEKICK_TARGETED_INPUT_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace keysidekick {

enum class TargetedMessageState {
    InitialDown,
    RepeatDown,
    KeyUp
};

struct TargetedKey {
    int usageId;
    std::uintptr_t target;
    std::uint16_t virtualKey;
    std::uint16_t scanCode;
    bool extended;
    std::uint64_t nextRepeatAtMs;
    std::uint32_t repeatIntervalMs;
};

class TargetedInputLedger {
public:
    bool recordDown(const TargetedKey& key);
    bool recordUp(int usageId, TargetedKey* released);
    bool owns(int usageId) const;

    std::vector<TargetedKey> dueRepeats(std::uint64_t nowMs);
    bool nextRepeatAt(std::uint64_t* deadlineMs) const;
    std::vector<TargetedKey> releaseAll();

    std::size_t size() const;
    bool empty() const;

private:
    std::vector<TargetedKey> heldKeys_;
};

std::uint32_t KeyboardRepeatDelayMs(unsigned int setting);
std::uint32_t KeyboardRepeatIntervalMs(unsigned int setting);
std::uint32_t TargetedMessageStateBits(TargetedMessageState state);

} // namespace keysidekick

#endif
