#ifndef KEYSIDEKICK_REPORT_DIFF_H
#define KEYSIDEKICK_REPORT_DIFF_H

// HID keyboard report diffing — the single edge detector shared by basic-mode
// re-injection and targeted-mode dispatch. Pure logic, no Win32: covered by
// tests/report_diff_tests.cpp.
//
// Report layout is the boot-protocol keyboard report:
//   byte 0    modifier bitmap
//   byte 1    reserved
//   bytes 2-7 up to six usage IDs (0 = empty slot)
//
// "Consumed" usages are the ones a profile mapping turned into an action (a
// keyboard macro, !switch/!launch, or a targeted key). They must never be
// re-injected, but they MUST stay in the tracked held-set: a consumed usage that
// is dropped from the tracked set looks like a brand-new key-down in the next
// report, which re-fires its action (re-launching the app, re-sending the
// macro) every time any other key changes state while it is held.

#include <cstddef>
#include <vector>

namespace keysidekick {

struct ReportEdges {
    std::vector<int> held;      // every usage held in this report (consumed included)
    std::vector<int> pressed;   // usages newly down in this report (consumed included)
    std::vector<int> released;  // usages held before this report and gone now
};

inline bool ReportEdgeConsumed(int usage, const std::vector<int>& consumed) {
    for (std::size_t i = 0; i < consumed.size(); ++i) {
        if (consumed[i] == usage) return true;
    }
    return false;
}

// prev: six slots of previously held usages (0 = empty slot)
// report/reportLength: raw HID report, at least 2 bytes
inline ReportEdges ComputeReportEdges(const int prev[6],
                                      const unsigned char* report,
                                      std::size_t reportLength) {
    ReportEdges edges;
    if (report == NULL || reportLength < 2) return edges;

    for (int slot = 0; slot < 6; ++slot) {
        const int usage = prev[slot];
        if (usage == 0) continue;
        bool stillHeld = false;
        for (std::size_t i = 2; i < 8 && i < reportLength; ++i) {
            if (report[i] == usage) { stillHeld = true; break; }
        }
        if (!stillHeld) edges.released.push_back(usage);
    }

    for (std::size_t i = 2; i < 8 && i < reportLength; ++i) {
        const int usage = report[i];
        if (usage == 0) continue;
        bool duplicate = false;
        for (std::size_t k = 0; k < edges.held.size(); ++k) {
            if (edges.held[k] == usage) { duplicate = true; break; }
        }
        if (duplicate) continue;
        bool wasHeld = false;
        for (int slot = 0; slot < 6; ++slot) {
            if (prev[slot] == usage) { wasHeld = true; break; }
        }
        edges.held.push_back(usage);
        if (!wasHeld) edges.pressed.push_back(usage);
    }
    return edges;
}

// Write the held set back into the fixed-size tracking array.
inline void StoreHeldUsages(const std::vector<int>& held, int out[6]) {
    for (int slot = 0; slot < 6; ++slot) {
        out[slot] = (slot < static_cast<int>(held.size())) ? held[static_cast<std::size_t>(slot)] : 0;
    }
}

}  // namespace keysidekick

#endif  // KEYSIDEKICK_REPORT_DIFF_H
