// report_diff_tests.cpp — edge detection for HID keyboard reports.
// Regression cover for: a consumed (action-mapped) key must stay in the tracked
// held-set, otherwise the next report re-detects it as a fresh key-down and the
// action fires again.

#include <cstdio>
#include <vector>

#include "../src/report_diff.h"

namespace {

int testsRun = 0;
int testsFailed = 0;

void Check(const char* testName, bool condition, const char* what) {
    ++testsRun;
    if (!condition) {
        ++testsFailed;
        std::printf("FAIL %s: %s\n", testName, what);
    }
}

std::vector<int> Held(const keysidekick::ReportEdges& edges) {
    return edges.held;
}

bool Contains(const std::vector<int>& values, int value) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == value) return true;
    }
    return false;
}

void TestFirstPressIsDetected() {
    const char* testName = "first press is detected";
    int prev[6] = {0, 0, 0, 0, 0, 0};
    const unsigned char report[8] = {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

    const keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(prev, report, sizeof(report));

    Check(testName, edges.pressed.size() == 1 && edges.pressed[0] == 0x04, "press edge reported once");
    Check(testName, edges.released.empty(), "nothing released");
    Check(testName, Held(edges).size() == 1 && Held(edges)[0] == 0x04, "held set tracks the key");
}

void TestConsumedKeyIsNotReDetectedWhileHeld() {
    const char* testName = "consumed key stays held across later reports";
    // Report 1: action key 0x3A pressed, profile consumed it (so it was never injected).
    int prev[6] = {0, 0, 0, 0, 0, 0};
    const unsigned char first[8] = {0x00, 0x00, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00};
    keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(prev, first, sizeof(first));
    Check(testName, edges.pressed.size() == 1 && edges.pressed[0] == 0x3A, "action key pressed once");
    keysidekick::StoreHeldUsages(edges.held, prev);

    // Report 2: the same action key is still held and another key joins it.
    const unsigned char second[8] = {0x00, 0x00, 0x3A, 0x07, 0x00, 0x00, 0x00, 0x00};
    edges = keysidekick::ComputeReportEdges(prev, second, sizeof(second));
    Check(testName, !Contains(edges.pressed, 0x3A), "held action key is NOT a new press edge");
    Check(testName, Contains(edges.pressed, 0x07), "the newly pressed key IS an edge");
    Check(testName, Contains(edges.held, 0x3A), "action key remains in the held set");
    keysidekick::StoreHeldUsages(edges.held, prev);

    // Report 3: action key released, other key still held.
    const unsigned char third[8] = {0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
    edges = keysidekick::ComputeReportEdges(prev, third, sizeof(third));
    Check(testName, Contains(edges.released, 0x3A), "release edge for the action key");
    Check(testName, edges.pressed.empty(), "no press edges while the second key is held");
}

void TestReleaseAndRepressFiresAgain() {
    const char* testName = "press -> hold -> release -> re-press cycle";
    int prev[6] = {0, 0, 0, 0, 0, 0};
    const unsigned char down[8] = {0x00, 0x00, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00};
    const unsigned char up[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(prev, down, sizeof(down));
    Check(testName, edges.pressed.size() == 1, "step 1: key-down is an edge");
    keysidekick::StoreHeldUsages(edges.held, prev);

    edges = keysidekick::ComputeReportEdges(prev, down, sizeof(down));
    Check(testName, edges.pressed.empty(), "step 2: the same held key is not a second edge");
    keysidekick::StoreHeldUsages(edges.held, prev);

    edges = keysidekick::ComputeReportEdges(prev, up, sizeof(up));
    Check(testName, edges.released.size() == 1 && edges.released[0] == 0x3A, "step 3: release is an edge");
    keysidekick::StoreHeldUsages(edges.held, prev);

    edges = keysidekick::ComputeReportEdges(prev, down, sizeof(down));
    Check(testName, edges.pressed.size() == 1 && edges.pressed[0] == 0x3A, "step 4: re-press fires again");
}

void TestRolloverAndEmptySlots() {
    const char* testName = "six-key rollover with gaps";
    int prev[6] = {0x04, 0x05, 0, 0, 0, 0};
    const unsigned char report[8] = {0x02, 0x00, 0x04, 0x00, 0x06, 0x07, 0x00, 0x00};

    const keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(prev, report, sizeof(report));

    Check(testName, edges.held.size() == 3, "three keys held");
    Check(testName, Contains(edges.released, 0x05), "dropped key released");
    Check(testName, Contains(edges.pressed, 0x06) && Contains(edges.pressed, 0x07), "new keys pressed");
    Check(testName, !Contains(edges.pressed, 0x04), "unchanged key is not an edge");
}

void TestDuplicateUsageAppearsOnce() {
    const char* testName = "duplicate usage in a report collapses";
    int prev[6] = {0, 0, 0, 0, 0, 0};
    const unsigned char report[8] = {0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00};

    const keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(prev, report, sizeof(report));

    Check(testName, edges.held.size() == 1, "one held entry");
    Check(testName, edges.pressed.size() == 1, "one press edge");
}

void TestShortAndNullReports() {
    const char* testName = "degenerate reports are safe";
    int prev[6] = {0x04, 0, 0, 0, 0, 0};
    const unsigned char shortReport[2] = {0x00, 0x00};

    const keysidekick::ReportEdges shortEdges =
        keysidekick::ComputeReportEdges(prev, shortReport, sizeof(shortReport));
    const keysidekick::ReportEdges nullEdges = keysidekick::ComputeReportEdges(prev, NULL, 8);

    Check(testName, shortEdges.held.empty() && shortEdges.pressed.empty(), "no key bytes, no edges");
    Check(testName, Contains(shortEdges.released, 0x04), "tracked key reported released");
    Check(testName, nullEdges.held.empty() && nullEdges.released.empty(), "null report yields nothing");
}

}  // namespace

int main() {
    TestFirstPressIsDetected();
    TestConsumedKeyIsNotReDetectedWhileHeld();
    TestReleaseAndRepressFiresAgain();
    TestRolloverAndEmptySlots();
    TestDuplicateUsageAppearsOnce();
    TestShortAndNullReports();

    std::printf("report_diff: %d checks, %d failed\n", testsRun, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
