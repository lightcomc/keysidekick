#include "../src/input_ledger.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using keysidekick::InputLedger;
using keysidekick::ScanKey;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;

    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

void TestDuplicateDown() {
    const char* testName = "duplicate down";
    InputLedger ledger;
    const ScanKey key(0x1E, false);

    REQUIRE(ledger.recordDown(key));
    REQUIRE(!ledger.recordDown(key));
    REQUIRE(ledger.size() == 1);
    REQUIRE(ledger.owns(key));
}

void TestOwnedUp() {
    const char* testName = "owned up";
    InputLedger ledger;
    const ScanKey key(0x1E, false);

    REQUIRE(ledger.recordDown(key));
    REQUIRE(ledger.recordUp(key));
    REQUIRE(!ledger.owns(key));
    REQUIRE(ledger.empty());
    REQUIRE(!ledger.recordUp(key));
}

void TestReleaseAllOrderingAndConfirmation() {
    const char* testName = "release all ordering and confirmation";
    InputLedger ledger;
    const ScanKey leftControl(0x1D, false);
    const ScanKey letterA(0x1E, false);
    const ScanKey rightControl(0x1D, true);

    REQUIRE(ledger.recordDown(leftControl));
    REQUIRE(ledger.recordDown(letterA));
    REQUIRE(ledger.recordDown(rightControl));

    const std::vector<ScanKey> releaseKeys = ledger.ownedKeysForRelease();
    REQUIRE(releaseKeys.size() == 3);
    REQUIRE(releaseKeys[0] == rightControl);
    REQUIRE(releaseKeys[1] == letterA);
    REQUIRE(releaseKeys[2] == leftControl);
    REQUIRE(ledger.size() == 3);

    REQUIRE(ledger.recordUp(releaseKeys[0]));
    REQUIRE(ledger.recordUp(releaseKeys[1]));
    REQUIRE(ledger.recordUp(releaseKeys[2]));
    REQUIRE(ledger.empty());
}

void TestNormalKeysAndModifiersRemainDistinct() {
    const char* testName = "normal keys and modifiers";
    InputLedger ledger;
    const ScanKey leftAlt(0x38, false);
    const ScanKey rightAlt(0x38, true);
    const ScanKey letterB(0x30, false);

    REQUIRE(ledger.recordDown(leftAlt));
    REQUIRE(ledger.recordDown(rightAlt));
    REQUIRE(ledger.recordDown(letterB));
    REQUIRE(ledger.owns(leftAlt));
    REQUIRE(ledger.owns(rightAlt));
    REQUIRE(ledger.owns(letterB));
    REQUIRE(ledger.size() == 3);
}

void TestNoUnownedRelease() {
    const char* testName = "no unowned release";
    InputLedger ledger;
    const ScanKey ownedKey(0x20, false);
    const ScanKey unownedKey(0x21, false);

    REQUIRE(ledger.recordDown(ownedKey));
    REQUIRE(!ledger.recordUp(unownedKey));
    REQUIRE(ledger.owns(ownedKey));
    REQUIRE(!ledger.owns(unownedKey));
    REQUIRE(ledger.size() == 1);
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"duplicate down", TestDuplicateDown},
        {"owned up", TestOwnedUp},
        {"release all ordering and confirmation", TestReleaseAllOrderingAndConfirmation},
        {"normal keys and modifiers", TestNormalKeysAndModifiersRemainDistinct},
        {"no unowned release", TestNoUnownedRelease},
    };

    for (const TestCase& test : tests) {
        test.run();
        std::cout << "PASS: " << test.name << '\n';
    }

    std::cout << "All input ledger tests passed (5/5).\n";
    return 0;
}
