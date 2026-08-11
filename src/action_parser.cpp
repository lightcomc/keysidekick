#include "action_parser.h"

#include <algorithm>
#include <cctype>

namespace keysidekick {
namespace action_parser {

namespace {

std::string toLower(const std::string& s) {
    std::string r(s);
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = (char)std::tolower((unsigned char)r[i]);
    return r;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool isMediaKey(const std::string& keyName) {
    std::string lower = toLower(keyName);
    return lower.find("media_") != std::string::npos ||
           lower.find("volume_") != std::string::npos;
}

} // namespace

bool ParseActionString(const std::string& str, Action* out) {
    if (!out || str.empty()) return false;

    // !switch:<profile>
    if (startsWith(str, "!switch:")) {
        *out = Action::switchProfile(str.substr(8));
        return true;
    }

    // !toggle:<profile>
    if (startsWith(str, "!toggle:")) {
        *out = Action::toggleProfile(str.substr(8), "");
        return true;
    }

    // !launch:<path>
    if (startsWith(str, "!launch:")) {
        *out = Action::launchApplication(str.substr(8));
        return true;
    }

    // !app:<name>:<keys>
    if (startsWith(str, "!app:")) {
        std::string rest = str.substr(5);
        std::size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            std::string appName = rest.substr(0, colon);
            std::string keys = rest.substr(colon + 1);
            // Nested action: parse keys as SendKey
            Action nested = Action::sendKey(0);
            nested.profileId = keys;
            *out = Action::sendToApplication(appName, nested);
            return true;
        }
        // Malformed !app: — treat as plain
    }

    // {KeyName} — bracket notation
    if (str.size() >= 2 && str[0] == '{' && str[str.size()-1] == '}') {
        std::string keyName = str.substr(1, str.size() - 2);
        if (isMediaKey(keyName)) {
            *out = Action::sendMediaKey(0);
        } else {
            *out = Action::sendKey(0);
        }
        out->profileId = str;  // preserve original for pass-through
        return true;
    }

    // Plain text (e.g. "1", "w", "W / Ц") — SendKey with text
    *out = Action::sendKey(0);
    out->profileId = str;
    return true;
}

std::string SerializeAction(const Action& action) {
    switch (action.type) {
        case ActionType::SwitchProfile:
            return "!switch:" + action.profileId;
        case ActionType::ToggleProfile:
            return "!toggle:" + action.profileId;
        case ActionType::LaunchApplication:
            return "!launch:" + action.applicationId;
        case ActionType::SendToApplication: {
            std::string keys;
            if (action.steps.size() == 1) {
                keys = action.steps[0].profileId;
            }
            return "!app:" + action.applicationId + ":" + keys;
        }
        case ActionType::SendKey:
        case ActionType::SendMediaKey:
        case ActionType::SendShortcut:
        case ActionType::Sequence:
        default:
            // For pass-through: profileId carries the original string
            return action.profileId;
    }
}

std::string ClassifyAction(const std::string& str) {
    if (startsWith(str, "!switch:")) return "switch";
    if (startsWith(str, "!toggle:")) return "toggle";
    if (startsWith(str, "!launch:")) return "launch";
    if (startsWith(str, "!app:")) return "multi-app";
    if (!str.empty() && str[0] == '{') {
        std::string lower = toLower(str);
        if (lower.find("media_") != std::string::npos ||
            lower.find("volume_") != std::string::npos)
            return "media";
        return "key";
    }
    return "key";
}

std::string DescribeAction(const std::string& str) {
    if (startsWith(str, "!switch:")) {
        return "Switch to " + str.substr(8);
    }
    if (startsWith(str, "!toggle:")) {
        return "Toggle " + str.substr(8);
    }
    if (startsWith(str, "!launch:")) {
        // Extract filename from path
        std::string path = str.substr(8);
        std::size_t slash = path.find_last_of("\\/");
        std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        // Remove .exe
        std::size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        return "Launch " + name;
    }
    if (startsWith(str, "!app:")) {
        std::string rest = str.substr(5);
        std::size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            std::string appName = rest.substr(0, colon);
            std::string keys = rest.substr(colon + 1);
            return keys + " → " + appName;
        }
    }
    // Plain key or {KeyName}
    return str;
}

} // namespace action_parser
} // namespace keysidekick
