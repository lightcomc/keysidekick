#ifndef KEYSIDEKICK_INPUT_LEDGER_H
#define KEYSIDEKICK_INPUT_LEDGER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace keysidekick {

struct ScanKey {
    std::uint16_t scan;
    bool extended;

    ScanKey(std::uint16_t scanCode, bool isExtended)
        : scan(scanCode), extended(isExtended) {}
};

bool operator==(const ScanKey& left, const ScanKey& right);
bool operator!=(const ScanKey& left, const ScanKey& right);

class InputLedger {
public:
    bool recordDown(const ScanKey& key);
    bool recordUp(const ScanKey& key);
    bool owns(const ScanKey& key) const;

    std::vector<ScanKey> ownedKeysForRelease() const;

    std::size_t size() const;
    bool empty() const;

private:
    std::vector<ScanKey> ownedKeys_;
};

} // namespace keysidekick

#endif
