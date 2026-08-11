#ifndef KEYSIDEKICK_CONFIG_DOMAIN_BRIDGE_H
#define KEYSIDEKICK_CONFIG_DOMAIN_BRIDGE_H

// Bridge between config::Config (INI storage model) and DomainModel (typed runtime model).
//
// Phase 2 pass-through: action strings (e.g. "{F1}", "!switch:basic") are carried
// verbatim — they are NOT parsed into typed Action objects yet (that is Phase 5).
// The raw string rides in Action::profileId as a documented carrier field, and
// ActionType is set to SendKey so domain validation passes (usageId = trigger key).
//
// Modifier conversion:
//   config 8-bit L/R mask (LCtrl=0x01 RCtrl=0x10 LShift=0x02 RShift=0x20 ...)
//   ↔ domain 4-bit generic mask (Ctrl=0x01 Shift=0x02 Alt=0x04 Meta=0x08)

#include <string>

#include "config_v3.h"
#include "domain_model.h"

namespace keysidekick {
namespace bridge {

// Convert storage Config → typed DomainModel.
// Applications become ApplicationTargets; profiles get linkedApplicationIds
// seeded from the single config application_id; mappings get stable generated ids.
DomainModel ConfigToDomain(const config::Config& config);

// Convert typed DomainModel → storage Config.
// Multi-app profiles are lossy-folded: only the default/first linked application
// is preserved as profile.application_id (config has no multi-app concept).
// Action strings are read from Action::profileId (pass-through carrier).
config::Config DomainToConfig(const DomainModel& model,
                              const config::GeneralSettings& general);

// ---- Modifier conversion helpers (exposed for projection layer) ----

// config 8-bit L/R modifier byte → domain 4-bit generic mask
unsigned char ExpandConfigModifierToDomain(unsigned char configMask);

// domain 4-bit generic mask → config 8-bit L/R modifier byte (both L+R set)
unsigned char ExpandDomainModifierToConfig(unsigned int domainMask);

} // namespace bridge
} // namespace keysidekick

#endif // KEYSIDEKICK_CONFIG_DOMAIN_BRIDGE_H
