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
    // Идентичность окна на момент захвата. HWND Windows переиспользует сразу
    // после DestroyWindow, поэтому одного target недостаточно: отложенный
    // repeat или key-up мог бы уйти чужому окну, унаследовавшему значение
    // дескриптора. pid+класс берутся при постановке на учёт и сверяются
    // перед каждой отправкой.
    std::uint32_t processId;
    std::uint64_t classHash;
};

// Совпадает ли идентичность окна с захваченной (оба поля обязаны совпасть).
inline bool WindowIdentityMatches(const TargetedKey& key,
                                  std::uint32_t processId,
                                  std::uint64_t classHash) {
    return key.processId == processId && key.classHash == classHash;
}

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

// FNV-1a 64 по ANSI-имени класса окна (NULL/пустая строка → базовое смещение).
std::uint64_t WindowClassHash(const char* className);

std::uint32_t KeyboardRepeatDelayMs(unsigned int setting);
std::uint32_t KeyboardRepeatIntervalMs(unsigned int setting);
std::uint32_t TargetedMessageStateBits(TargetedMessageState state);

} // namespace keysidekick

#endif
