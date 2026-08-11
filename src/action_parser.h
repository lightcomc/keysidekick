#ifndef KEYSIDEKICK_ACTION_PARSER_H
#define KEYSIDEKICK_ACTION_PARSER_H

// Action string ↔ typed Action conversion.
// Parses the legacy action grammar ({F1}, !switch:basic, !app:name:keys, etc.)
// into typed domain_model::Action objects, and serializes them back.

#include <string>
#include "domain_model.h"

namespace keysidekick {
namespace action_parser {

// Parse an action string into a typed Action.
// Returns true on success. On failure, action.type is unchanged and
// the string is treated as a plain "send key" text action.
//
// Grammar:
//   {F1} / {Media_Play_Pause} / {Space}  → SendKey/SendMediaKey (key name in profileId)
//   !switch:<profile>                    → SwitchProfile
//   !toggle:<profile>                    → ToggleProfile (secondary = "")
//   !launch:<path>                       → LaunchApplication (path in applicationId)
//   !app:<name>:<keys>                   → SendToApplication (nested SendKey)
//   <plain text>                         → SendKey (text in profileId, e.g. "1", "w")
bool ParseActionString(const std::string& str, Action* out);

// Serialize a typed Action back into an action string.
// Inverse of ParseActionString.
std::string SerializeAction(const Action& action);

// Classify an action string for display purposes.
// Returns a human-readable category: "key", "media", "switch", "toggle",
// "launch", "multi-app", or "key" for plain text.
std::string ClassifyAction(const std::string& str);

// Human-readable description for dashboard display.
// Examples: "Next track → Spotify", "Switch to basic", "Launch AIMP"
std::string DescribeAction(const std::string& str);

} // namespace action_parser
} // namespace keysidekick

#endif // KEYSIDEKICK_ACTION_PARSER_H
