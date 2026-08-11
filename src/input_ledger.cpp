#include "input_ledger.h"

#include <algorithm>

namespace keysidekick {

bool operator==(const ScanKey& left, const ScanKey& right) {
    return left.scan == right.scan && left.extended == right.extended;
}

bool operator!=(const ScanKey& left, const ScanKey& right) {
    return !(left == right);
}

bool InputLedger::recordDown(const ScanKey& key) {
    if (owns(key)) return false;

    ownedKeys_.push_back(key);
    return true;
}

bool InputLedger::recordUp(const ScanKey& key) {
    const std::vector<ScanKey>::iterator ownedKey =
        std::find(ownedKeys_.begin(), ownedKeys_.end(), key);
    if (ownedKey == ownedKeys_.end()) return false;

    ownedKeys_.erase(ownedKey);
    return true;
}

bool InputLedger::owns(const ScanKey& key) const {
    return std::find(ownedKeys_.begin(), ownedKeys_.end(), key) != ownedKeys_.end();
}

std::vector<ScanKey> InputLedger::ownedKeysForRelease() const {
    return std::vector<ScanKey>(ownedKeys_.rbegin(), ownedKeys_.rend());
}

std::size_t InputLedger::size() const {
    return ownedKeys_.size();
}

bool InputLedger::empty() const {
    return ownedKeys_.empty();
}

} // namespace keysidekick
