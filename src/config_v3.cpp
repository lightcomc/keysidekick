#include "config_v3.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace keysidekick {
namespace config {

Diagnostic::Diagnostic()
    : severity(DIAGNOSTIC_WARNING), line(0) {
}

bool Diagnostic::operator==(const Diagnostic& other) const {
    return severity == other.severity && line == other.line &&
           section == other.section && key == other.key && message == other.message;
}

GeneralSettings::GeneralSettings()
    : http_port(8765), http_enabled(true), tray_enabled(true), enable_log(true) {
}

bool GeneralSettings::operator==(const GeneralSettings& other) const {
    return device_vid_pid == other.device_vid_pid &&
           default_profile_id == other.default_profile_id &&
           http_port == other.http_port && http_enabled == other.http_enabled &&
           tray_enabled == other.tray_enabled && enable_log == other.enable_log;
}

Application::Application()
    : auto_start(false) {
}

bool Application::operator==(const Application& other) const {
    return id == other.id && name == other.name &&
           target_class == other.target_class && target_exe == other.target_exe &&
           target_path == other.target_path && auto_start == other.auto_start &&
           default_profile_id == other.default_profile_id;
}

Mapping::Mapping()
    : usage_id(0), modifiers(0) {
}

bool Mapping::operator==(const Mapping& other) const {
    return usage_id == other.usage_id && modifiers == other.modifiers &&
           action == other.action;
}

Profile::Profile()
    : mode(PROFILE_MODE_BASIC), built_in(false) {
}

bool Profile::operator==(const Profile& other) const {
    return id == other.id && name == other.name && mode == other.mode &&
           built_in == other.built_in && application_id == other.application_id &&
           layer_modifier == other.layer_modifier &&
           mappings == other.mappings;
}

bool Extension::operator==(const Extension& other) const {
    return section == other.section && key == other.key && value == other.value;
}

Config::Config()
    : schema_version(3) {
}

bool Config::operator==(const Config& other) const {
    return schema_version == other.schema_version && general == other.general &&
           applications == other.applications && profiles == other.profiles &&
           extensions == other.extensions;
}

bool ParseResult::ok() const {
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (diagnostics[index].severity == DIAGNOSTIC_ERROR) return false;
    }
    return true;
}

bool SerializeResult::ok() const {
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (diagnostics[index].severity == DIAGNOSTIC_ERROR) return false;
    }
    return true;
}

namespace {

struct IniEntry {
    std::size_t line;
    std::string section;
    std::string key;
    std::string value;
};

struct RawDocument {
    std::vector<IniEntry> entries;
    std::vector<Diagnostic> diagnostics;
};

struct LegacyProfile {
    std::string name;
    ProfileMode mode;
    bool mode_seen;
    std::string target_class;
    std::string target_exe;
    std::string target_path;
    bool auto_start;
    bool target_seen;
    std::vector<Mapping> mappings;

    LegacyProfile()
        : mode(PROFILE_MODE_TARGETED), mode_seen(false), auto_start(false),
          target_seen(false) {
    }
};

struct V3Profile {
    Profile profile;
    bool seen;

    V3Profile() : seen(false) {
    }
};

struct V3Application {
    Application application;
    bool seen;

    V3Application() : seen(false) {
    }
};

struct ExtensionRecord {
    Extension extension;
    bool has_section;
    bool has_key;
    bool has_value;

    ExtensionRecord() : has_section(false), has_key(false), has_value(false) {
    }
};

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' ||
            value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string Lower(const std::string& value) {
    std::string result = value;
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(result[index])));
    }
    return result;
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return Lower(left) == Lower(right);
}

bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           EqualsIgnoreCase(value.substr(0, prefix.size()), prefix);
}

void AddDiagnostic(std::vector<Diagnostic>& diagnostics,
                   DiagnosticSeverity severity,
                   std::size_t line,
                   const std::string& section,
                   const std::string& key,
                   const std::string& message) {
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.line = line;
    diagnostic.section = section;
    diagnostic.key = key;
    diagnostic.message = message;
    diagnostics.push_back(diagnostic);
}

void AddExtension(Config& config,
                  const std::string& section,
                  const std::string& key,
                  const std::string& value) {
    Extension extension;
    extension.section = section;
    extension.key = key;
    extension.value = value;
    config.extensions.push_back(extension);
}

RawDocument Tokenize(const std::string& text) {
    RawDocument document;
    std::istringstream input(text);
    std::string current_section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        if (trimmed[0] == '[') {
            if (trimmed.size() >= 2 && trimmed[trimmed.size() - 1] == ']') {
                const std::string section = Trim(trimmed.substr(1, trimmed.size() - 2));
                if (section.empty()) {
                    AddDiagnostic(document.diagnostics, DIAGNOSTIC_WARNING, line_number,
                                  current_section, std::string(),
                                  "Empty section name was preserved as malformed input");
                    IniEntry entry;
                    entry.line = line_number;
                    entry.section = "$document";
                    entry.key = "$raw-line-" + std::to_string(line_number);
                    entry.value = line;
                    document.entries.push_back(entry);
                } else {
                    current_section = section;
                }
            } else {
                AddDiagnostic(document.diagnostics, DIAGNOSTIC_WARNING, line_number,
                              current_section, std::string(),
                              "Malformed section header was preserved");
                IniEntry entry;
                entry.line = line_number;
                entry.section = "$document";
                entry.key = "$raw-line-" + std::to_string(line_number);
                entry.value = line;
                document.entries.push_back(entry);
            }
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            AddDiagnostic(document.diagnostics, DIAGNOSTIC_WARNING, line_number,
                          current_section, std::string(),
                          "Malformed line without '=' was preserved");
            IniEntry entry;
            entry.line = line_number;
            entry.section = current_section.empty() ? "$document" : current_section;
            entry.key = "$raw-line-" + std::to_string(line_number);
            entry.value = line;
            document.entries.push_back(entry);
            continue;
        }

        IniEntry entry;
        entry.line = line_number;
        entry.section = current_section;
        entry.key = Trim(line.substr(0, equals));
        entry.value = line.substr(equals + 1);
        if (entry.key.empty()) {
            AddDiagnostic(document.diagnostics, DIAGNOSTIC_WARNING, line_number,
                          current_section, std::string(),
                          "Empty key was preserved");
            entry.key = "$empty-key-line-" + std::to_string(line_number);
        }
        if (current_section.empty()) {
            AddDiagnostic(document.diagnostics, DIAGNOSTIC_WARNING, line_number,
                          std::string(), entry.key,
                          "Field outside a section was preserved");
        }
        document.entries.push_back(entry);
    }
    return document;
}

bool ParseInteger(const std::string& text, int minimum, int maximum, int& result) {
    const std::string value = Trim(text);
    if (value.empty()) return false;
    char* end = NULL;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    result = static_cast<int>(parsed);
    return true;
}

bool ParseBoolean(const std::string& text, bool& result) {
    const std::string value = Lower(Trim(text));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        result = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        result = false;
        return true;
    }
    return false;
}

bool IsHexDigit(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return value - 'A' + 10;
}

std::string Unescape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            result.push_back(value[index]);
            continue;
        }
        const char escaped = value[++index];
        if (escaped == 'n') result.push_back('\n');
        else if (escaped == 'r') result.push_back('\r');
        else if (escaped == 't') result.push_back('\t');
        else if (escaped == '0') result.push_back('\0');
        else if (escaped == '\\') result.push_back('\\');
        else if (escaped == '[') result.push_back('[');
        else if (escaped == ']') result.push_back(']');
        else if (escaped == '=') result.push_back('=');
        else if (escaped == 'x' && index + 2 < value.size() &&
                 IsHexDigit(value[index + 1]) && IsHexDigit(value[index + 2])) {
            const int byte = HexValue(value[index + 1]) * 16 + HexValue(value[index + 2]);
            result.push_back(static_cast<char>(byte));
            index += 2;
        } else {
            result.push_back('\\');
            result.push_back(escaped);
        }
    }
    return result;
}

std::string Escape(const std::string& value) {
    std::ostringstream result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte == '\\') result << "\\\\";
        else if (byte == '\n') result << "\\n";
        else if (byte == '\r') result << "\\r";
        else if (byte == '\t') result << "\\t";
        else if (byte == '\0') result << "\\0";
        else if (byte == '[') result << "\\[";
        else if (byte == ']') result << "\\]";
        else if (byte == '=') result << "\\=";
        else if (byte < 0x20 || byte == 0x7F) {
            result << "\\x" << std::uppercase << std::hex << std::setw(2)
                   << std::setfill('0') << static_cast<int>(byte) << std::dec;
        } else {
            result << static_cast<char>(byte);
        }
    }
    return result.str();
}

bool ParseModifierToken(const std::string& token, unsigned char& mask) {
    if (EqualsIgnoreCase(token, "Ctrl")) mask |= 0x01 | 0x10;
    else if (EqualsIgnoreCase(token, "Shift")) mask |= 0x02 | 0x20;
    else if (EqualsIgnoreCase(token, "Alt")) mask |= 0x04 | 0x40;
    else if (EqualsIgnoreCase(token, "Win")) mask |= 0x08 | 0x80;
    else if (EqualsIgnoreCase(token, "LCtrl")) mask |= 0x01;
    else if (EqualsIgnoreCase(token, "LShift")) mask |= 0x02;
    else if (EqualsIgnoreCase(token, "LAlt")) mask |= 0x04;
    else if (EqualsIgnoreCase(token, "LWin")) mask |= 0x08;
    else if (EqualsIgnoreCase(token, "RCtrl")) mask |= 0x10;
    else if (EqualsIgnoreCase(token, "RShift")) mask |= 0x20;
    else if (EqualsIgnoreCase(token, "RAlt")) mask |= 0x40;
    else if (EqualsIgnoreCase(token, "RWin")) mask |= 0x80;
    else return false;
    return true;
}

bool ParseKeySpec(const std::string& key,
                  Mapping& mapping,
                  std::string& error) {
    const std::size_t first_plus = key.find('+');
    const std::string usage_part = Trim(
        first_plus == std::string::npos ? key : key.substr(0, first_plus));
    int usage_id = 0;
    if (StartsWithIgnoreCase(usage_part, "USAGE_")) {
        const std::string hex = usage_part.substr(6);
        if (hex.empty()) {
            error = "Invalid empty HID usage";
            return false;
        }
        for (std::size_t index = 0; index < hex.size(); ++index) {
            if (!IsHexDigit(hex[index])) {
                error = "Invalid hexadecimal HID usage";
                return false;
            }
        }
        char* end = NULL;
        const long parsed = std::strtol(hex.c_str(), &end, 16);
        if (end == hex.c_str() || *end != '\0' || parsed < 1 || parsed > 255) {
            error = "HID usage must be between 1 and 255";
            return false;
        }
        usage_id = static_cast<int>(parsed);
    } else if (!ParseInteger(usage_part, 1, 255, usage_id)) {
        error = "Invalid decimal HID usage";
        return false;
    }

    unsigned char modifier_mask = 0;
    std::size_t position = first_plus;
    while (position != std::string::npos) {
        const std::size_t next = key.find('+', position + 1);
        const std::string token = Trim(key.substr(
            position + 1,
            next == std::string::npos ? std::string::npos : next - position - 1));
        if (token.empty() || !ParseModifierToken(token, modifier_mask)) {
            error = "Unknown or empty modifier '" + token + "'";
            return false;
        }
        position = next;
    }

    mapping.usage_id = usage_id;
    mapping.modifiers = modifier_mask;
    return true;
}

std::string ModifierSuffix(unsigned char mask) {
    std::string result;
    if ((mask & 0x11) == 0x11) result += "+Ctrl";
    else {
        if (mask & 0x01) result += "+LCtrl";
        if (mask & 0x10) result += "+RCtrl";
    }
    if ((mask & 0x22) == 0x22) result += "+Shift";
    else {
        if (mask & 0x02) result += "+LShift";
        if (mask & 0x20) result += "+RShift";
    }
    if ((mask & 0x44) == 0x44) result += "+Alt";
    else {
        if (mask & 0x04) result += "+LAlt";
        if (mask & 0x40) result += "+RAlt";
    }
    if ((mask & 0x88) == 0x88) result += "+Win";
    else {
        if (mask & 0x08) result += "+LWin";
        if (mask & 0x80) result += "+RWin";
    }
    return result;
}

std::string KeySpec(const Mapping& mapping) {
    std::ostringstream result;
    result << "USAGE_" << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << mapping.usage_id << std::dec;
    result << ModifierSuffix(mapping.modifiers);
    return result.str();
}

std::uint64_t StableHash(const std::string& value) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t index = 0; index < value.size(); ++index) {
        hash ^= static_cast<unsigned char>(value[index]);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string HashText(const std::string& value) {
    std::ostringstream result;
    result << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
           << StableHash(value);
    return result.str();
}

std::string Slug(const std::string& value) {
    std::string result;
    bool last_dash = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (std::isalnum(byte)) {
            result.push_back(static_cast<char>(std::tolower(byte)));
            last_dash = false;
        } else if (!result.empty() && !last_dash) {
            result.push_back('-');
            last_dash = true;
        }
    }
    while (!result.empty() && result[result.size() - 1] == '-') result.erase(result.size() - 1);
    if (result.empty()) result = "item";
    if (result.size() > 32) result.erase(32);
    return result;
}

std::string MigratedProfileId(const std::string& name) {
    return "profile-" + Slug(name) + "-" + HashText("profile\n" + Lower(name)).substr(0, 12);
}

std::string MigratedApplicationId(const LegacyProfile& profile) {
    const std::string seed = "application\n" + Lower(profile.name) + "\n" +
        profile.target_class + "\n" + profile.target_exe + "\n" +
        profile.target_path + "\n" + (profile.auto_start ? "1" : "0");
    return "app-" + Slug(profile.name) + "-" + HashText(seed).substr(0, 12);
}

bool IsSafeId(const std::string& id) {
    if (id.empty()) return false;
    for (std::size_t index = 0; index < id.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(id[index]);
        if (!(std::isalnum(byte) || byte == '-' || byte == '_')) return false;
    }
    return true;
}

bool ApplicationLess(const Application& left, const Application& right) {
    return left.id < right.id;
}

bool ProfileLess(const Profile& left, const Profile& right) {
    return left.id < right.id;
}

bool MappingLess(const Mapping& left, const Mapping& right) {
    if (left.usage_id != right.usage_id) return left.usage_id < right.usage_id;
    if (left.modifiers != right.modifiers) return left.modifiers < right.modifiers;
    return left.action < right.action;
}

bool ExtensionLess(const Extension& left, const Extension& right) {
    if (left.section != right.section) return left.section < right.section;
    if (left.key != right.key) return left.key < right.key;
    return left.value < right.value;
}

bool HasSchemaVersion3(const RawDocument& document, int& declared_version) {
    declared_version = 0;
    for (std::size_t index = 0; index < document.entries.size(); ++index) {
        const IniEntry& entry = document.entries[index];
        if (EqualsIgnoreCase(entry.section, "General") &&
            EqualsIgnoreCase(entry.key, "SchemaVersion")) {
            int version = 0;
            if (ParseInteger(entry.value, 1, INT_MAX, version)) declared_version = version;
            return version == 3;
        }
    }
    return false;
}

void PreserveUnknown(ParseResult& result,
                     const IniEntry& entry,
                     const std::string& value,
                     const std::string& message) {
    AddExtension(result.config, entry.section.empty() ? "$document" : entry.section,
                 entry.key, value);
    AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, entry.line,
                  entry.section, entry.key, message);
}

void ParseGeneralField(ParseResult& result,
                       const IniEntry& entry,
                       const std::string& value,
                       bool versioned,
                       std::string& default_token,
                       bool& default_is_id) {
    if (EqualsIgnoreCase(entry.key, "SchemaVersion")) {
        int version = 0;
        if (!ParseInteger(value, 1, INT_MAX, version)) {
            PreserveUnknown(result, entry, value,
                            "Invalid SchemaVersion was preserved");
        }
    } else if (EqualsIgnoreCase(entry.key, "DeviceVIDPID")) {
        result.config.general.device_vid_pid = value;
    } else if (EqualsIgnoreCase(entry.key, "DefaultProfileId")) {
        default_token = value;
        default_is_id = true;
    } else if (EqualsIgnoreCase(entry.key, "DefaultProfile")) {
        default_token = value;
        default_is_id = false;
        if (versioned) {
            AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, entry.line,
                          entry.section, entry.key,
                          "Legacy DefaultProfile name was migrated to a profile id");
        }
    } else if (EqualsIgnoreCase(entry.key, "HTTPPort")) {
        int port = 0;
        if (ParseInteger(value, 1, 65535, port)) {
            result.config.general.http_port = port;
        } else {
            PreserveUnknown(result, entry, value,
                            "Invalid HTTPPort was preserved; default retained");
        }
    } else if (EqualsIgnoreCase(entry.key, "HTTPEnabled")) {
        bool enabled = false;
        if (ParseBoolean(value, enabled)) result.config.general.http_enabled = enabled;
        else PreserveUnknown(result, entry, value,
                             "Invalid HTTPEnabled was preserved; default retained");
    } else if (EqualsIgnoreCase(entry.key, "TrayEnabled")) {
        bool enabled = false;
        if (ParseBoolean(value, enabled)) result.config.general.tray_enabled = enabled;
        else PreserveUnknown(result, entry, value,
                             "Invalid TrayEnabled was preserved; default retained");
    } else if (EqualsIgnoreCase(entry.key, "EnableLog")) {
        bool enabled = false;
        if (ParseBoolean(value, enabled)) result.config.general.enable_log = enabled;
        else PreserveUnknown(result, entry, value,
                             "Invalid EnableLog was preserved; default retained");
    } else {
        PreserveUnknown(result, entry, value, "Unknown General field was preserved");
    }
}

void EnsureMigratedBasic(std::map<std::string, LegacyProfile>& profiles) {
    for (std::map<std::string, LegacyProfile>::const_iterator it = profiles.begin();
         it != profiles.end(); ++it) {
        if (EqualsIgnoreCase(it->second.name, "basic")) return;
    }
    LegacyProfile basic;
    basic.name = "basic";
    basic.mode = PROFILE_MODE_BASIC;
    basic.mode_seen = true;
    profiles[Lower(basic.name)] = basic;
}

ParseResult ParseLegacy(const RawDocument& document, int declared_version) {
    ParseResult result;
    result.config.schema_version = 3;
    result.diagnostics = document.diagnostics;
    std::map<std::string, LegacyProfile> profiles;
    std::string default_name;
    bool ignored_default_is_id = false;
    bool legacy_aimp_seen = false;

    if (declared_version != 0 && declared_version != 2) {
        AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, 0, "General",
                      "SchemaVersion",
                      "Unsupported legacy schema was interpreted as v2 and migrated");
    } else {
        AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, 0, "General",
                      "SchemaVersion",
                      "Configuration without SchemaVersion=3 was migrated to schema v3");
    }

    for (std::size_t index = 0; index < document.entries.size(); ++index) {
        const IniEntry& entry = document.entries[index];
        const std::string value = Trim(entry.value);
        if (EqualsIgnoreCase(entry.section, "General")) {
            ParseGeneralField(result, entry, value, false, default_name,
                              ignored_default_is_id);
            continue;
        }
        if (EqualsIgnoreCase(entry.section, "AIMP")) {
            legacy_aimp_seen = true;
            LegacyProfile& profile = profiles["default"];
            profile.name = "default";
            profile.mode = PROFILE_MODE_TARGETED;
            profile.mode_seen = true;
            if (EqualsIgnoreCase(entry.key, "TargetClass")) {
                profile.target_class = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "TargetExe")) {
                profile.target_exe = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "TargetPath")) {
                profile.target_path = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "AutoStart")) {
                bool auto_start = false;
                if (ParseBoolean(value, auto_start)) {
                    profile.auto_start = auto_start;
                    profile.target_seen = true;
                } else {
                    PreserveUnknown(result, entry, value,
                                    "Invalid legacy AutoStart was preserved");
                }
            } else if (EqualsIgnoreCase(entry.key, "EnableLog")) {
                bool enabled = false;
                if (ParseBoolean(value, enabled)) result.config.general.enable_log = enabled;
                else PreserveUnknown(result, entry, value,
                                     "Invalid legacy EnableLog was preserved");
            } else {
                PreserveUnknown(result, entry, value,
                                "Unknown legacy AIMP field was preserved");
            }
            continue;
        }
        if (EqualsIgnoreCase(entry.section, "Keys")) {
            legacy_aimp_seen = true;
            LegacyProfile& profile = profiles["default"];
            profile.name = "default";
            profile.mode = PROFILE_MODE_TARGETED;
            profile.mode_seen = true;
            Mapping mapping;
            std::string error;
            if (ParseKeySpec(entry.key, mapping, error) && !value.empty()) {
                mapping.action = value;
                profile.mappings.push_back(mapping);
            } else {
                PreserveUnknown(result, entry, value,
                                error.empty() ? "Empty legacy action was preserved" :
                                                error + "; mapping was preserved as an extension");
            }
            continue;
        }

        if (StartsWithIgnoreCase(entry.section, "Profile.")) {
            const std::string rest = entry.section.substr(8);
            const std::string lower_rest = Lower(rest);
            const bool keys_section = lower_rest.size() > 5 &&
                lower_rest.substr(lower_rest.size() - 5) == ".keys";
            const bool mappings_section = lower_rest.size() > 9 &&
                lower_rest.substr(lower_rest.size() - 9) == ".mappings";
            std::string profile_name = rest;
            if (keys_section) profile_name = rest.substr(0, rest.size() - 5);
            else if (mappings_section) profile_name = rest.substr(0, rest.size() - 9);
            if (profile_name.empty() || profile_name.find('.') != std::string::npos) {
                PreserveUnknown(result, entry, value,
                                "Unknown profile subsection was preserved");
                continue;
            }
            LegacyProfile& profile = profiles[Lower(profile_name)];
            profile.name = profile_name;
            if (keys_section || mappings_section) {
                Mapping mapping;
                std::string error;
                if (ParseKeySpec(entry.key, mapping, error) && !value.empty()) {
                    mapping.action = value;
                    profile.mappings.push_back(mapping);
                } else {
                    PreserveUnknown(result, entry, value,
                                    error.empty() ? "Empty action was preserved" :
                                                    error + "; mapping was preserved as an extension");
                }
            } else if (EqualsIgnoreCase(entry.key, "Mode")) {
                if (EqualsIgnoreCase(value, "basic") || EqualsIgnoreCase(value, "normal")) {
                    profile.mode = PROFILE_MODE_BASIC;
                    profile.mode_seen = true;
                } else if (EqualsIgnoreCase(value, "targeted")) {
                    profile.mode = PROFILE_MODE_TARGETED;
                    profile.mode_seen = true;
                } else {
                    PreserveUnknown(result, entry, value,
                                    "Unknown profile Mode was preserved; targeted retained");
                }
            } else if (EqualsIgnoreCase(entry.key, "TargetClass")) {
                profile.target_class = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "TargetExe")) {
                profile.target_exe = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "TargetPath")) {
                profile.target_path = value;
                profile.target_seen = true;
            } else if (EqualsIgnoreCase(entry.key, "AutoStart")) {
                bool auto_start = false;
                if (ParseBoolean(value, auto_start)) {
                    profile.auto_start = auto_start;
                    profile.target_seen = true;
                } else {
                    PreserveUnknown(result, entry, value,
                                    "Invalid profile AutoStart was preserved");
                }
            } else {
                PreserveUnknown(result, entry, value,
                                "Unknown profile field was preserved");
            }
            continue;
        }

        PreserveUnknown(result, entry, value,
                        entry.section.empty() ? "Field outside a section was preserved" :
                                                "Unknown section or field was preserved");
    }

    if (legacy_aimp_seen && default_name.empty()) default_name = "default";
    EnsureMigratedBasic(profiles);

    std::map<std::string, std::string> profile_ids_by_name;
    for (std::map<std::string, LegacyProfile>::iterator it = profiles.begin();
         it != profiles.end(); ++it) {
        LegacyProfile& source = it->second;
        if (source.name.empty()) source.name = it->first;
        Profile profile;
        profile.id = MigratedProfileId(source.name);
        profile.name = source.name;
        profile.mode = source.mode;
        profile.built_in = source.mode == PROFILE_MODE_BASIC;
        profile.mappings = source.mappings;
        std::sort(profile.mappings.begin(), profile.mappings.end(), MappingLess);
        profile_ids_by_name[Lower(source.name)] = profile.id;

        if (source.mode == PROFILE_MODE_TARGETED) {
            Application application;
            application.id = MigratedApplicationId(source);
            application.name = source.name;
            application.target_class = source.target_class;
            application.target_exe = source.target_exe;
            application.target_path = source.target_path;
            application.auto_start = source.auto_start;
            application.default_profile_id = profile.id;
            profile.application_id = application.id;
            result.config.applications.push_back(application);
            AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, 0,
                          "Profile." + source.name, std::string(),
                          "Migrated targeted profile fields into Application '" +
                              application.id + "'");
        } else if (source.target_seen) {
            IniEntry synthetic;
            synthetic.section = "Profile." + source.name;
            synthetic.key = "$target-fields";
            const std::string combined = source.target_class + "|" + source.target_exe +
                "|" + source.target_path + "|" + (source.auto_start ? "1" : "0");
            PreserveUnknown(result, synthetic, combined,
                            "Target fields on a basic profile were preserved as an extension");
        }
        result.config.profiles.push_back(profile);
    }

    std::sort(result.config.applications.begin(), result.config.applications.end(),
              ApplicationLess);
    std::sort(result.config.profiles.begin(), result.config.profiles.end(), ProfileLess);
    std::sort(result.config.extensions.begin(), result.config.extensions.end(), ExtensionLess);

    if (default_name.empty()) default_name = "basic";
    const std::map<std::string, std::string>::const_iterator default_profile =
        profile_ids_by_name.find(Lower(default_name));
    if (default_profile != profile_ids_by_name.end()) {
        result.config.general.default_profile_id = default_profile->second;
    } else {
        result.config.general.default_profile_id = MigratedProfileId(default_name);
        AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, 0, "General",
                      "DefaultProfile",
                      "Default profile name does not match a parsed profile; stable id retained");
    }
    return result;
}

std::string V3Value(const IniEntry& entry) {
    return Unescape(entry.value);
}

void ParseV3ProfileField(ParseResult& result,
                         V3Profile& target,
                         const IniEntry& entry,
                         const std::string& value) {
    if (EqualsIgnoreCase(entry.key, "Name")) {
        target.profile.name = value;
    } else if (EqualsIgnoreCase(entry.key, "Mode")) {
        if (EqualsIgnoreCase(value, "basic") || EqualsIgnoreCase(value, "normal")) {
            target.profile.mode = PROFILE_MODE_BASIC;
        } else if (EqualsIgnoreCase(value, "targeted")) {
            target.profile.mode = PROFILE_MODE_TARGETED;
        } else {
            PreserveUnknown(result, entry, value,
                            "Unknown profile Mode was preserved; basic retained");
        }
    } else if (EqualsIgnoreCase(entry.key, "BuiltIn")) {
        bool built_in = false;
        if (ParseBoolean(value, built_in)) target.profile.built_in = built_in;
        else PreserveUnknown(result, entry, value,
                             "Invalid BuiltIn value was preserved");
    } else if (EqualsIgnoreCase(entry.key, "ApplicationId")) {
        target.profile.application_id = value;
    } else if (EqualsIgnoreCase(entry.key, "LayerMod")) {
        target.profile.layer_modifier = value;
    } else {
        PreserveUnknown(result, entry, value, "Unknown Profile field was preserved");
    }
}

void ParseV3ApplicationField(ParseResult& result,
                             V3Application& target,
                             const IniEntry& entry,
                             const std::string& value) {
    if (EqualsIgnoreCase(entry.key, "Name")) {
        target.application.name = value;
    } else if (EqualsIgnoreCase(entry.key, "TargetClass")) {
        target.application.target_class = value;
    } else if (EqualsIgnoreCase(entry.key, "TargetExe")) {
        target.application.target_exe = value;
    } else if (EqualsIgnoreCase(entry.key, "TargetPath")) {
        target.application.target_path = value;
    } else if (EqualsIgnoreCase(entry.key, "AutoStart")) {
        bool auto_start = false;
        if (ParseBoolean(value, auto_start)) target.application.auto_start = auto_start;
        else PreserveUnknown(result, entry, value,
                             "Invalid AutoStart value was preserved");
    } else if (EqualsIgnoreCase(entry.key, "DefaultProfileId")) {
        target.application.default_profile_id = value;
    } else {
        PreserveUnknown(result, entry, value,
                        "Unknown Application field was preserved");
    }
}

ParseResult ParseV3(const RawDocument& document) {
    ParseResult result;
    result.config.schema_version = 3;
    result.diagnostics = document.diagnostics;
    std::map<std::string, V3Profile> profiles;
    std::map<std::string, V3Application> applications;
    std::map<std::string, ExtensionRecord> extension_records;
    std::string default_token;
    bool default_is_id = true;

    for (std::size_t index = 0; index < document.entries.size(); ++index) {
        const IniEntry& entry = document.entries[index];
        const std::string value = V3Value(entry);
        if (EqualsIgnoreCase(entry.section, "General")) {
            ParseGeneralField(result, entry, value, true, default_token, default_is_id);
            continue;
        }
        if (StartsWithIgnoreCase(entry.section, "Application.")) {
            const std::string id = entry.section.substr(12);
            if (!IsSafeId(id)) {
                PreserveUnknown(result, entry, value,
                                "Unsafe Application id was preserved as an extension");
                continue;
            }
            V3Application& application = applications[id];
            application.seen = true;
            application.application.id = id;
            ParseV3ApplicationField(result, application, entry, value);
            continue;
        }
        if (StartsWithIgnoreCase(entry.section, "Profile.")) {
            const std::string rest = entry.section.substr(8);
            const std::string lower_rest = Lower(rest);
            const bool mappings_section = lower_rest.size() > 9 &&
                lower_rest.substr(lower_rest.size() - 9) == ".mappings";
            const bool keys_section = lower_rest.size() > 5 &&
                lower_rest.substr(lower_rest.size() - 5) == ".keys";
            std::string id = rest;
            if (mappings_section) id = rest.substr(0, rest.size() - 9);
            else if (keys_section) id = rest.substr(0, rest.size() - 5);
            if (!IsSafeId(id)) {
                PreserveUnknown(result, entry, value,
                                "Unsafe Profile id was preserved as an extension");
                continue;
            }
            V3Profile& profile = profiles[id];
            profile.seen = true;
            profile.profile.id = id;
            if (mappings_section || keys_section) {
                Mapping mapping;
                std::string error;
                if (ParseKeySpec(entry.key, mapping, error) && !value.empty()) {
                    mapping.action = value;
                    profile.profile.mappings.push_back(mapping);
                    if (keys_section) {
                        AddDiagnostic(result.diagnostics, DIAGNOSTIC_WARNING, entry.line,
                                      entry.section, entry.key,
                                      "Legacy .Keys section was accepted as .Mappings");
                    }
                } else {
                    PreserveUnknown(result, entry, value,
                                    error.empty() ? "Empty mapping action was preserved" :
                                                    error + "; mapping was preserved as an extension");
                }
            } else if (rest.find('.') != std::string::npos) {
                PreserveUnknown(result, entry, value,
                                "Unknown Profile subsection was preserved");
            } else {
                ParseV3ProfileField(result, profile, entry, value);
            }
            continue;
        }
        if (StartsWithIgnoreCase(entry.section, "Extension.")) {
            const std::string record_id = entry.section.substr(10);
            ExtensionRecord& record = extension_records[record_id];
            if (EqualsIgnoreCase(entry.key, "Section")) {
                record.extension.section = value;
                record.has_section = true;
            } else if (EqualsIgnoreCase(entry.key, "Key")) {
                record.extension.key = value;
                record.has_key = true;
            } else if (EqualsIgnoreCase(entry.key, "Value")) {
                record.extension.value = value;
                record.has_value = true;
            } else {
                PreserveUnknown(result, entry, value,
                                "Unknown Extension record field was preserved");
            }
            continue;
        }

        PreserveUnknown(result, entry, value,
                        entry.section.empty() ? "Field outside a section was preserved" :
                                                "Unknown section or field was preserved");
    }

    for (std::map<std::string, V3Application>::iterator it = applications.begin();
         it != applications.end(); ++it) {
        result.config.applications.push_back(it->second.application);
    }
    for (std::map<std::string, V3Profile>::iterator it = profiles.begin();
         it != profiles.end(); ++it) {
        Profile profile = it->second.profile;
        if (profile.name.empty()) profile.name = profile.id;
        std::sort(profile.mappings.begin(), profile.mappings.end(), MappingLess);
        result.config.profiles.push_back(profile);
    }
    for (std::map<std::string, ExtensionRecord>::iterator it = extension_records.begin();
         it != extension_records.end(); ++it) {
        if (it->second.has_section && it->second.has_key && it->second.has_value) {
            result.config.extensions.push_back(it->second.extension);
        } else {
            IniEntry synthetic;
            synthetic.section = "Extension." + it->first;
            synthetic.key = "$incomplete";
            synthetic.value = "Incomplete extension record";
            PreserveUnknown(result, synthetic, synthetic.value,
                            "Incomplete Extension record was preserved");
        }
    }

    if (default_is_id) {
        result.config.general.default_profile_id = default_token;
    } else {
        for (std::size_t index = 0; index < result.config.profiles.size(); ++index) {
            if (EqualsIgnoreCase(result.config.profiles[index].name, default_token)) {
                result.config.general.default_profile_id = result.config.profiles[index].id;
                break;
            }
        }
        if (result.config.general.default_profile_id.empty()) {
            result.config.general.default_profile_id = default_token;
        }
    }

    std::sort(result.config.applications.begin(), result.config.applications.end(),
              ApplicationLess);
    std::sort(result.config.profiles.begin(), result.config.profiles.end(), ProfileLess);
    std::sort(result.config.extensions.begin(), result.config.extensions.end(), ExtensionLess);
    return result;
}

bool ContainsProfile(const std::set<std::string>& profile_ids, const std::string& id) {
    return !id.empty() && profile_ids.find(id) != profile_ids.end();
}

void ValidateForSerialization(const Config& config,
                              std::vector<Diagnostic>& diagnostics) {
    if (config.schema_version != 3) {
        AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0, "General", "SchemaVersion",
                      "Only SchemaVersion=3 can be serialized");
    }

    std::set<std::string> profile_ids;
    for (std::size_t index = 0; index < config.profiles.size(); ++index) {
        const Profile& profile = config.profiles[index];
        if (!IsSafeId(profile.id)) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0, "Profile", profile.id,
                          "Profile id must contain only letters, digits, '-' or '_'");
        } else if (!profile_ids.insert(profile.id).second) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0, "Profile", profile.id,
                          "Duplicate Profile id");
        }
        for (std::size_t mapping_index = 0;
             mapping_index < profile.mappings.size(); ++mapping_index) {
            if (profile.mappings[mapping_index].usage_id < 1 ||
                profile.mappings[mapping_index].usage_id > 255) {
                AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0,
                              "Profile." + profile.id + ".Mappings", std::string(),
                              "Mapping HID usage must be between 1 and 255");
            }
        }
    }

    std::set<std::string> application_ids;
    for (std::size_t index = 0; index < config.applications.size(); ++index) {
        const Application& application = config.applications[index];
        if (!IsSafeId(application.id)) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0, "Application", application.id,
                          "Application id must contain only letters, digits, '-' or '_'");
        } else if (!application_ids.insert(application.id).second) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_ERROR, 0, "Application", application.id,
                          "Duplicate Application id");
        }
    }

    if (!config.general.default_profile_id.empty() &&
        !ContainsProfile(profile_ids, config.general.default_profile_id)) {
        AddDiagnostic(diagnostics, DIAGNOSTIC_WARNING, 0, "General", "DefaultProfileId",
                      "DefaultProfileId does not reference a serialized Profile");
    }
    for (std::size_t index = 0; index < config.profiles.size(); ++index) {
        const Profile& profile = config.profiles[index];
        if (!profile.application_id.empty() &&
            application_ids.find(profile.application_id) == application_ids.end()) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_WARNING, 0,
                          "Profile." + profile.id, "ApplicationId",
                          "ApplicationId does not reference a serialized Application");
        }
    }
    for (std::size_t index = 0; index < config.applications.size(); ++index) {
        const Application& application = config.applications[index];
        if (!application.default_profile_id.empty() &&
            !ContainsProfile(profile_ids, application.default_profile_id)) {
            AddDiagnostic(diagnostics, DIAGNOSTIC_WARNING, 0,
                          "Application." + application.id, "DefaultProfileId",
                          "DefaultProfileId does not reference a serialized Profile");
        }
    }
}

void WriteField(std::ostringstream& output,
                const std::string& key,
                const std::string& value) {
    output << key << '=' << Escape(value) << '\n';
}

} // namespace

ParseResult Parse(const std::string& text) {
    const RawDocument document = Tokenize(text);
    int declared_version = 0;
    if (HasSchemaVersion3(document, declared_version)) return ParseV3(document);
    return ParseLegacy(document, declared_version);
}

SerializeResult Serialize(const Config& config) {
    SerializeResult result;
    ValidateForSerialization(config, result.diagnostics);
    if (!result.ok()) return result;

    std::vector<Application> applications = config.applications;
    std::vector<Profile> profiles = config.profiles;
    std::vector<Extension> extensions = config.extensions;
    std::sort(applications.begin(), applications.end(), ApplicationLess);
    std::sort(profiles.begin(), profiles.end(), ProfileLess);
    std::sort(extensions.begin(), extensions.end(), ExtensionLess);
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        std::sort(profiles[index].mappings.begin(), profiles[index].mappings.end(), MappingLess);
    }

    std::ostringstream output;
    output << "[General]\n";
    output << "SchemaVersion=3\n";
    WriteField(output, "DeviceVIDPID", config.general.device_vid_pid);
    WriteField(output, "DefaultProfileId", config.general.default_profile_id);
    output << "HTTPPort=" << config.general.http_port << '\n';
    output << "HTTPEnabled=" << (config.general.http_enabled ? 1 : 0) << '\n';
    output << "TrayEnabled=" << (config.general.tray_enabled ? 1 : 0) << '\n';
    output << "EnableLog=" << (config.general.enable_log ? 1 : 0) << "\n\n";

    for (std::size_t index = 0; index < applications.size(); ++index) {
        const Application& application = applications[index];
        output << "[Application." << application.id << "]\n";
        WriteField(output, "Name", application.name);
        WriteField(output, "TargetClass", application.target_class);
        WriteField(output, "TargetExe", application.target_exe);
        WriteField(output, "TargetPath", application.target_path);
        output << "AutoStart=" << (application.auto_start ? 1 : 0) << '\n';
        WriteField(output, "DefaultProfileId", application.default_profile_id);
        output << '\n';
    }

    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const Profile& profile = profiles[index];
        output << "[Profile." << profile.id << "]\n";
        WriteField(output, "Name", profile.name);
        output << "Mode="
               << (profile.mode == PROFILE_MODE_BASIC ? "basic" : "targeted") << '\n';
        output << "BuiltIn=" << (profile.built_in ? 1 : 0) << '\n';
        WriteField(output, "ApplicationId", profile.application_id);
        if (!profile.layer_modifier.empty()) WriteField(output, "LayerMod", profile.layer_modifier);
        output << '\n';
        if (!profile.mappings.empty()) {
            output << "[Profile." << profile.id << ".Mappings]\n";
            for (std::size_t mapping_index = 0;
                 mapping_index < profile.mappings.size(); ++mapping_index) {
                output << KeySpec(profile.mappings[mapping_index]) << '='
                       << Escape(profile.mappings[mapping_index].action) << '\n';
            }
            output << '\n';
        }
    }

    for (std::size_t index = 0; index < extensions.size(); ++index) {
        output << "[Extension." << std::setw(4) << std::setfill('0') << (index + 1)
               << "]\n";
        WriteField(output, "Section", extensions[index].section);
        WriteField(output, "Key", extensions[index].key);
        WriteField(output, "Value", extensions[index].value);
        output << '\n';
    }

    result.text = output.str();
    return result;
}

} // namespace config
} // namespace keysidekick
