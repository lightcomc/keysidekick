#ifndef KEYSIDEKICK_CONFIG_V3_H
#define KEYSIDEKICK_CONFIG_V3_H

#include <string>
#include <vector>

namespace keysidekick {
namespace config {

enum DiagnosticSeverity {
    DIAGNOSTIC_WARNING = 0,
    DIAGNOSTIC_ERROR = 1
};

struct Diagnostic {
    DiagnosticSeverity severity;
    std::size_t line;
    std::string section;
    std::string key;
    std::string message;

    Diagnostic();
    bool operator==(const Diagnostic& other) const;
};

enum ProfileMode {
    PROFILE_MODE_BASIC = 0,
    PROFILE_MODE_TARGETED = 1
};

struct GeneralSettings {
    std::string device_vid_pid;
    std::string default_profile_id;
    int http_port;
    bool http_enabled;
    bool tray_enabled;
    bool enable_log;

    GeneralSettings();
    bool operator==(const GeneralSettings& other) const;
};

struct Application {
    std::string id;
    std::string name;
    std::string target_class;
    std::string target_exe;
    std::string target_path;
    bool auto_start;
    std::string default_profile_id;

    Application();
    bool operator==(const Application& other) const;
};

struct Mapping {
    int usage_id;
    unsigned char modifiers;
    std::string action;

    Mapping();
    bool operator==(const Mapping& other) const;
};

struct Profile {
    std::string id;
    std::string name;
    ProfileMode mode;
    bool built_in;
    std::string application_id;
    std::string layer_modifier;   // "Alt"/"Ctrl"/"Shift"/"Win"/"" — Fn-слой для профиля
    std::vector<Mapping> mappings;

    Profile();
    bool operator==(const Profile& other) const;
};

struct Extension {
    std::string section;
    std::string key;
    std::string value;

    bool operator==(const Extension& other) const;
};

struct Config {
    int schema_version;
    GeneralSettings general;
    std::vector<Application> applications;
    std::vector<Profile> profiles;
    std::vector<Extension> extensions;

    Config();
    bool operator==(const Config& other) const;
};

struct ParseResult {
    Config config;
    std::vector<Diagnostic> diagnostics;

    bool ok() const;
};

struct SerializeResult {
    std::string text;
    std::vector<Diagnostic> diagnostics;

    bool ok() const;
};

ParseResult Parse(const std::string& text);
SerializeResult Serialize(const Config& config);

} // namespace config
} // namespace keysidekick

#endif // KEYSIDEKICK_CONFIG_V3_H
