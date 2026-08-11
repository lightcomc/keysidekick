#include "../src/config_v3.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using keysidekick::config::Application;
using keysidekick::config::Config;
using keysidekick::config::Diagnostic;
using keysidekick::config::Extension;
using keysidekick::config::Mapping;
using keysidekick::config::Parse;
using keysidekick::config::ParseResult;
using keysidekick::config::PROFILE_MODE_BASIC;
using keysidekick::config::PROFILE_MODE_TARGETED;
using keysidekick::config::Profile;
using keysidekick::config::Serialize;
using keysidekick::config::SerializeResult;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;
    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

std::string ReadFixture(const char* fileName) {
    const std::string candidates[] = {
        std::string("tests/fixtures/") + fileName,
        std::string("fixtures/") + fileName,
    };
    for (std::size_t index = 0; index < 2; ++index) {
        std::ifstream input(candidates[index].c_str(), std::ios::binary);
        if (!input) continue;
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }
    return std::string();
}

const Profile* FindProfileByName(const Config& config, const std::string& name) {
    for (std::size_t index = 0; index < config.profiles.size(); ++index) {
        if (config.profiles[index].name == name) return &config.profiles[index];
    }
    return NULL;
}

const Application* FindApplicationById(const Config& config, const std::string& id) {
    for (std::size_t index = 0; index < config.applications.size(); ++index) {
        if (config.applications[index].id == id) return &config.applications[index];
    }
    return NULL;
}

bool HasDiagnosticContaining(const std::vector<Diagnostic>& diagnostics,
                             const std::string& text) {
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (diagnostics[index].message.find(text) != std::string::npos) return true;
    }
    return false;
}

Mapping MakeMapping(int usageId, unsigned char modifiers, const std::string& action) {
    Mapping mapping;
    mapping.usage_id = usageId;
    mapping.modifiers = modifiers;
    mapping.action = action;
    return mapping;
}

void TestMigratesV2MultiProfileFixture() {
    const char* testName = "migrates v2 multi-profile fixture";
    const std::string input = ReadFixture("legacy_multi_profile.ini");
    REQUIRE(!input.empty());

    const ParseResult result = Parse(input);
    REQUIRE(result.ok());
    REQUIRE(result.config.schema_version == 3);
    REQUIRE(result.config.general.device_vid_pid == "vid_1234&pid_abcd");
    REQUIRE(result.config.general.http_port == 9001);
    REQUIRE(result.config.general.http_enabled);
    REQUIRE(!result.config.general.tray_enabled);
    REQUIRE(result.config.general.enable_log);
    REQUIRE(result.config.profiles.size() == 3);
    REQUIRE(result.config.applications.size() == 2);

    const Profile* basic = FindProfileByName(result.config, "basic");
    const Profile* editor = FindProfileByName(result.config, "editor");
    const Profile* presenter = FindProfileByName(result.config, "presenter");
    REQUIRE(basic != NULL);
    REQUIRE(editor != NULL);
    REQUIRE(presenter != NULL);
    REQUIRE(basic->mode == PROFILE_MODE_BASIC);
    REQUIRE(basic->built_in);
    REQUIRE(editor->mode == PROFILE_MODE_TARGETED);
    REQUIRE(result.config.general.default_profile_id == editor->id);
    REQUIRE(!editor->application_id.empty());
    REQUIRE(!presenter->application_id.empty());
    REQUIRE(editor->application_id != presenter->application_id);
    REQUIRE(editor->mappings.size() == 2);
    REQUIRE(editor->mappings[0].action == "!launch:C:\\Tools\\HelperDemo.exe");
    REQUIRE(editor->mappings[0].modifiers == static_cast<unsigned char>(0x33));

    const Application* editorApp = FindApplicationById(result.config, editor->application_id);
    REQUIRE(editorApp != NULL);
    REQUIRE(editorApp->target_class == "GenericEditorWindow");
    REQUIRE(editorApp->target_exe == "EditorDemo.exe");
    REQUIRE(editorApp->target_path == "C:\\Tools\\EditorDemo.exe");
    REQUIRE(editorApp->auto_start);
    REQUIRE(editorApp->default_profile_id == editor->id);
}

void TestMigratesLegacyAimpAndKeys() {
    const char* testName = "migrates legacy AIMP and Keys";
    const std::string input =
        "[AIMP]\n"
        "TargetClass=GenericPlayerWindow\n"
        "TargetExe=PlayerDemo.exe\n"
        "TargetPath=C:\\Apps\\PlayerDemo.exe\n"
        "AutoStart=1\n"
        "EnableLog=0\n"
        "\n"
        "[Keys]\n"
        "USAGE_14={F1}\n"
        "30+Alt=!toggle:default\n";

    const ParseResult result = Parse(input);
    REQUIRE(result.ok());
    const Profile* profile = FindProfileByName(result.config, "default");
    REQUIRE(profile != NULL);
    REQUIRE(profile->mode == PROFILE_MODE_TARGETED);
    REQUIRE(result.config.general.default_profile_id == profile->id);
    REQUIRE(!result.config.general.enable_log);
    REQUIRE(profile->mappings.size() == 2);
    REQUIRE(profile->mappings[0].usage_id == 0x14);
    REQUIRE(profile->mappings[1].usage_id == 30);
    REQUIRE(profile->mappings[1].modifiers == static_cast<unsigned char>(0x44));
    const Application* application = FindApplicationById(result.config, profile->application_id);
    REQUIRE(application != NULL);
    REQUIRE(application->target_exe == "PlayerDemo.exe");
    REQUIRE(application->default_profile_id == profile->id);
}

Config MakeV3Config() {
    Config config;
    config.schema_version = 3;
    config.general.device_vid_pid = "vid_4321&pid_dcba";
    config.general.default_profile_id = "profile-editor";
    config.general.http_port = 8123;
    config.general.http_enabled = true;
    config.general.tray_enabled = true;
    config.general.enable_log = false;

    Application editorApp;
    editorApp.id = "app-editor";
    editorApp.name = "Generic Editor";
    editorApp.target_class = "GenericEditorWindow";
    editorApp.target_exe = "EditorDemo.exe";
    editorApp.target_path = "C:\\Tools\\EditorDemo.exe";
    editorApp.auto_start = true;
    editorApp.default_profile_id = "profile-editor";
    config.applications.push_back(editorApp);

    Application meetingApp;
    meetingApp.id = "app-meeting";
    meetingApp.name = "Conference Demo";
    meetingApp.target_class = "GenericMeetingWindow";
    meetingApp.target_exe = "MeetingDemo.exe";
    meetingApp.target_path = "C:\\Tools\\MeetingDemo.exe";
    meetingApp.auto_start = false;
    meetingApp.default_profile_id = "profile-meeting";
    config.applications.push_back(meetingApp);

    Profile basic;
    basic.id = "profile-basic";
    basic.name = "basic";
    basic.mode = PROFILE_MODE_BASIC;
    basic.built_in = true;
    basic.mappings.push_back(MakeMapping(0x29, 0, "!switch:profile-editor"));
    config.profiles.push_back(basic);

    Profile editor;
    editor.id = "profile-editor";
    editor.name = "Editor Controls";
    editor.mode = PROFILE_MODE_TARGETED;
    editor.application_id = "app-editor";
    editor.mappings.push_back(MakeMapping(0x04, 0x33, "!launch:C:\\Tools\\HelperDemo.exe"));
    editor.mappings.push_back(MakeMapping(0x15, 0, "!app:app-meeting:{Media_Next_Track}"));
    config.profiles.push_back(editor);

    Profile meeting;
    meeting.id = "profile-meeting";
    meeting.name = "Meeting Controls";
    meeting.mode = PROFILE_MODE_TARGETED;
    meeting.application_id = "app-meeting";
    meeting.mappings.push_back(MakeMapping(0x2C, 0x04, "{Media_Play_Pause}"));
    config.profiles.push_back(meeting);

    Extension extension;
    extension.section = "Plugin.Generic";
    extension.key = "Enabled";
    extension.value = "future-value";
    config.extensions.push_back(extension);
    return config;
}

void TestV3MultiApplicationRoundTrip() {
    const char* testName = "v3 multi-application round trip";
    const Config original = MakeV3Config();
    const SerializeResult serialized = Serialize(original);
    REQUIRE(serialized.ok());
    REQUIRE(serialized.text.find("SchemaVersion=3") != std::string::npos);
    REQUIRE(serialized.text.find("[Application.app-editor]") != std::string::npos);
    REQUIRE(serialized.text.find("[Profile.profile-editor.Mappings]") != std::string::npos);
    REQUIRE(serialized.text.find("[Extension.0001]") != std::string::npos);

    const ParseResult reparsed = Parse(serialized.text);
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.config == original);
    const SerializeResult serializedAgain = Serialize(reparsed.config);
    REQUIRE(serializedAgain.ok());
    REQUIRE(serializedAgain.text == serialized.text);
}

void TestPreservesModifiersAndActions() {
    const char* testName = "preserves modifiers and actions";
    const std::string input =
        "[General]\nDefaultProfile=macro\n"
        "[Profile.macro]\nMode=targeted\nTargetExe=MacroDemo.exe\n"
        "[Profile.macro.Keys]\n"
        "USAGE_04+Ctrl+Shift=!switch:basic\n"
        "USAGE_05+LAlt+RWin=!toggle:macro\n"
        "USAGE_06+RCtrl=!launch:C:\\Tools\\MacroDemo.exe\n"
        "USAGE_07=!app:OtherDemo:{Media_Play_Pause}\n"
        "USAGE_08={F12}\n";
    const ParseResult parsed = Parse(input);
    REQUIRE(parsed.ok());
    const Profile* profile = FindProfileByName(parsed.config, "macro");
    REQUIRE(profile != NULL);
    REQUIRE(profile->mappings.size() == 5);
    REQUIRE(profile->mappings[0].modifiers == static_cast<unsigned char>(0x33));
    REQUIRE(profile->mappings[1].modifiers == static_cast<unsigned char>(0x84));
    REQUIRE(profile->mappings[2].modifiers == static_cast<unsigned char>(0x10));
    REQUIRE(profile->mappings[3].action == "!app:OtherDemo:{Media_Play_Pause}");
    REQUIRE(profile->mappings[4].action == "{F12}");

    const SerializeResult serialized = Serialize(parsed.config);
    REQUIRE(serialized.ok());
    REQUIRE(serialized.text.find("USAGE_04+Ctrl+Shift=!switch:basic") != std::string::npos);
    REQUIRE(serialized.text.find("USAGE_05+LAlt+RWin=!toggle:macro") != std::string::npos);
    REQUIRE(serialized.text.find("USAGE_06+RCtrl=!launch:C:\\\\Tools\\\\MacroDemo.exe") != std::string::npos);
    const ParseResult reparsed = Parse(serialized.text);
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.config == parsed.config);
}

void TestReportsMalformedAndEscapesValues() {
    const char* testName = "reports malformed input and escapes values";
    const std::string malformed =
        "orphan=value\n"
        "[General]\n"
        "HTTPPort=not-a-number\n"
        "UnknownGeneral=future\n"
        "[Profile.bad]\n"
        "Mode=targeted\n"
        "TargetExe=BadDemo.exe\n"
        "[Profile.bad.Keys]\n"
        "USAGE_04+Hyper={F1}\n"
        "not a key value\n"
        "[Future.Section]\n"
        "Option=kept\n"
        "[Broken\n";
    const ParseResult parsed = Parse(malformed);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.diagnostics.size() >= 5);
    REQUIRE(HasDiagnosticContaining(parsed.diagnostics, "outside a section"));
    REQUIRE(HasDiagnosticContaining(parsed.diagnostics, "HTTPPort"));
    REQUIRE(HasDiagnosticContaining(parsed.diagnostics, "modifier"));
    REQUIRE(HasDiagnosticContaining(parsed.diagnostics, "Unknown"));
    REQUIRE(parsed.config.extensions.size() >= 3);

    Config unsafe = MakeV3Config();
    unsafe.applications[0].target_path = "]\n[Injected]\nOwned=1";
    unsafe.profiles[1].mappings[0].action = "line one\n[Injected.Action]\nOwned=1";
    const SerializeResult serialized = Serialize(unsafe);
    REQUIRE(serialized.ok());
    REQUIRE(serialized.text.find("\n[Injected]\n") == std::string::npos);
    REQUIRE(serialized.text.find("\n[Injected.Action]\n") == std::string::npos);
    REQUIRE(serialized.text.find("\\n") != std::string::npos);
    const ParseResult reparsed = Parse(serialized.text);
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.config == unsafe);

    unsafe.profiles[0].id = "bad]id";
    const SerializeResult rejected = Serialize(unsafe);
    REQUIRE(!rejected.ok());
    REQUIRE(rejected.text.empty());
}

void TestDeterministicMigrationIdsAndOutput() {
    const char* testName = "deterministic migration ids and output";
    const std::string first =
        "[General]\nDefaultProfile=alpha\n"
        "[Profile.alpha]\nMode=targeted\nTargetExe=AlphaDemo.exe\n"
        "[Profile.beta]\nMode=targeted\nTargetExe=BetaDemo.exe\n";
    const std::string second =
        "[Profile.beta]\nTargetExe=BetaDemo.exe\nMode=targeted\n"
        "[Profile.alpha]\nTargetExe=AlphaDemo.exe\nMode=targeted\n"
        "[General]\nDefaultProfile=alpha\n";
    const ParseResult firstParsed = Parse(first);
    const ParseResult secondParsed = Parse(second);
    REQUIRE(firstParsed.ok());
    REQUIRE(secondParsed.ok());

    const Profile* firstAlpha = FindProfileByName(firstParsed.config, "alpha");
    const Profile* secondAlpha = FindProfileByName(secondParsed.config, "alpha");
    REQUIRE(firstAlpha != NULL);
    REQUIRE(secondAlpha != NULL);
    REQUIRE(firstAlpha->id == secondAlpha->id);
    REQUIRE(firstAlpha->application_id == secondAlpha->application_id);
    REQUIRE(firstAlpha->id.find("profile-alpha-") == 0);
    REQUIRE(firstAlpha->application_id.find("app-alpha-") == 0);

    const SerializeResult firstSerialized = Serialize(firstParsed.config);
    const SerializeResult secondSerialized = Serialize(secondParsed.config);
    REQUIRE(firstSerialized.ok());
    REQUIRE(secondSerialized.ok());
    REQUIRE(firstSerialized.text == secondSerialized.text);
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"migrates v2 multi-profile fixture", TestMigratesV2MultiProfileFixture},
        {"migrates legacy AIMP and Keys", TestMigratesLegacyAimpAndKeys},
        {"v3 multi-application round trip", TestV3MultiApplicationRoundTrip},
        {"preserves modifiers and actions", TestPreservesModifiersAndActions},
        {"reports malformed input and escapes values", TestReportsMalformedAndEscapesValues},
        {"deterministic migration ids and output", TestDeterministicMigrationIdsAndOutput},
    };

    for (std::size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        tests[index].run();
        std::cout << "PASS: " << tests[index].name << '\n';
    }
    std::cout << "All config v3 tests passed (6/6).\n";
    return 0;
}
