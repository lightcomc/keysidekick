#include "../src/targeted_input.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using keysidekick::KeyboardRepeatDelayMs;
using keysidekick::KeyboardRepeatIntervalMs;
using keysidekick::TargetedInputLedger;
using keysidekick::TargetedKey;
using keysidekick::TargetedMessageState;
using keysidekick::TargetedMessageStateBits;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;

    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

TargetedKey MakeKey(int usageId,
                    std::uintptr_t target,
                    std::uint16_t virtualKey,
                    std::uint64_t nextRepeatAtMs,
                    std::uint32_t repeatIntervalMs) {
    TargetedKey key = {
        usageId,
        target,
        virtualKey,
        static_cast<std::uint16_t>(virtualKey + 1),
        false,
        nextRepeatAtMs,
        repeatIntervalMs
    };
    return key;
}

void TestDuplicateDownIsRejected() {
    const char* testName = "duplicate targeted down";
    TargetedInputLedger ledger;
    const TargetedKey key = MakeKey(0x1A, 1001, 0x57, 500, 50);

    REQUIRE(ledger.recordDown(key));
    REQUIRE(!ledger.recordDown(key));
    REQUIRE(ledger.owns(0x1A));
    REQUIRE(ledger.size() == 1);
}

void TestReleaseReturnsOriginalTarget() {
    const char* testName = "release returns original target";
    TargetedInputLedger ledger;
    const TargetedKey key = MakeKey(0x04, 0xABCDEF, 0x41, 900, 40);
    TargetedKey released = MakeKey(0, 0, 0, 0, 0);

    REQUIRE(ledger.recordDown(key));
    REQUIRE(ledger.recordUp(0x04, &released));
    REQUIRE(released.usageId == key.usageId);
    REQUIRE(released.target == key.target);
    REQUIRE(released.virtualKey == key.virtualKey);
    REQUIRE(released.scanCode == key.scanCode);
    REQUIRE(ledger.empty());
    REQUIRE(!ledger.recordUp(0x04, &released));
}

void TestRepeatSchedulingAvoidsBursts() {
    const char* testName = "repeat scheduling avoids bursts";
    TargetedInputLedger ledger;
    REQUIRE(ledger.recordDown(MakeKey(0x05, 2002, 0x42, 1000, 100)));

    REQUIRE(ledger.dueRepeats(999).empty());

    const std::vector<TargetedKey> firstDue = ledger.dueRepeats(1000);
    REQUIRE(firstDue.size() == 1);
    REQUIRE(firstDue[0].usageId == 0x05);

    std::uint64_t nextDeadline = 0;
    REQUIRE(ledger.nextRepeatAt(&nextDeadline));
    REQUIRE(nextDeadline == 1100);
    REQUIRE(ledger.dueRepeats(1099).empty());

    const std::vector<TargetedKey> delayedDue = ledger.dueRepeats(1250);
    REQUIRE(delayedDue.size() == 1);
    REQUIRE(ledger.nextRepeatAt(&nextDeadline));
    REQUIRE(nextDeadline == 1350);
}

void TestNextDeadlineUsesEarliestHold() {
    const char* testName = "next deadline uses earliest hold";
    TargetedInputLedger ledger;
    std::uint64_t deadline = 0;

    REQUIRE(!ledger.nextRepeatAt(&deadline));
    REQUIRE(ledger.recordDown(MakeKey(0x06, 3003, 0x43, 1200, 80)));
    REQUIRE(ledger.recordDown(MakeKey(0x07, 3003, 0x44, 750, 80)));
    REQUIRE(ledger.nextRepeatAt(&deadline));
    REQUIRE(deadline == 750);
}

void TestReleaseAllIsReverseAndClears() {
    const char* testName = "release all is reverse and clears";
    TargetedInputLedger ledger;
    REQUIRE(ledger.recordDown(MakeKey(0x04, 1, 0x41, 100, 50)));
    REQUIRE(ledger.recordDown(MakeKey(0x05, 1, 0x42, 100, 50)));
    REQUIRE(ledger.recordDown(MakeKey(0x06, 1, 0x43, 100, 50)));

    const std::vector<TargetedKey> released = ledger.releaseAll();
    REQUIRE(released.size() == 3);
    REQUIRE(released[0].usageId == 0x06);
    REQUIRE(released[1].usageId == 0x05);
    REQUIRE(released[2].usageId == 0x04);
    REQUIRE(ledger.empty());
    REQUIRE(ledger.releaseAll().empty());
}

void TestKeyboardRepeatSettingsAreClamped() {
    const char* testName = "keyboard repeat settings are clamped";
    REQUIRE(KeyboardRepeatDelayMs(0) == 250);
    REQUIRE(KeyboardRepeatDelayMs(1) == 500);
    REQUIRE(KeyboardRepeatDelayMs(3) == 1000);
    REQUIRE(KeyboardRepeatDelayMs(99) == 1000);
    REQUIRE(KeyboardRepeatIntervalMs(0) == 400);
    REQUIRE(KeyboardRepeatIntervalMs(10) == 88);
    REQUIRE(KeyboardRepeatIntervalMs(31) == 33);
    REQUIRE(KeyboardRepeatIntervalMs(99) == 33);
}

void TestMessageStateBitsMatchWin32Contract() {
    const char* testName = "message state bits match Win32 contract";
    REQUIRE(TargetedMessageStateBits(TargetedMessageState::InitialDown) == 0u);
    REQUIRE(TargetedMessageStateBits(TargetedMessageState::RepeatDown) == (1u << 30));
    REQUIRE(TargetedMessageStateBits(TargetedMessageState::KeyUp) ==
            ((1u << 30) | (1u << 31)));
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"duplicate targeted down", TestDuplicateDownIsRejected},
        {"release returns original target", TestReleaseReturnsOriginalTarget},
        {"repeat scheduling avoids bursts", TestRepeatSchedulingAvoidsBursts},
        {"next deadline uses earliest hold", TestNextDeadlineUsesEarliestHold},
        {"release all is reverse and clears", TestReleaseAllIsReverseAndClears},
        {"keyboard repeat settings are clamped", TestKeyboardRepeatSettingsAreClamped},
        {"message state bits match Win32 contract", TestMessageStateBitsMatchWin32Contract},
    };

    for (const TestCase& test : tests) {
        test.run();
        std::cout << "PASS: " << test.name << '\n';
    }

    std::cout << "All targeted input tests passed (7/7).\n";
    return 0;
}
