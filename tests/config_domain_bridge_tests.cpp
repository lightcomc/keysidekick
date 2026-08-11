// Bridge round-trip tests: config::Config → DomainModel → config::Config
// Verifies action strings, applications, profiles, modifiers survive the round-trip.
#include <cassert>
#include <cstdio>
#include <string>

#include "../src/config_domain_bridge.h"

using keysidekick::DomainModel;
using keysidekick::ApplicationTarget;
using keysidekick::ProfileMode;
using keysidekick::ModifierCtrl;
using keysidekick::ModifierShift;
using keysidekick::ModifierAlt;
using keysidekick::ModifierMeta;
using keysidekick::ModifierNone;
using keysidekick::config::Config;
using keysidekick::config::GeneralSettings;
using keysidekick::config::Application;  // config::Application
// NOTE: Profile and Mapping collide between keysidekick:: and keysidekick::config::
using keysidekick::config::Parse;
using keysidekick::config::Serialize;
typedef keysidekick::config::Profile CProfile;
typedef keysidekick::config::Mapping CMapping;

static int g_tests = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    ++g_tests; \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failed; \
    } \
} while(0)

// Build a config mimicking the live config.ini (AIMP + basic)
static Config MakeAimpConfig() {
    Config c;
    c.schema_version = 3;
    c.general.device_vid_pid = "vid_1234&pid_abcd";
    c.general.default_profile_id = "basic";
    c.general.http_port = 8765;
    c.general.http_enabled = true;
    c.general.tray_enabled = true;
    c.general.enable_log = true;

    // aimp profile + its application
    Application aimpApp;
    aimpApp.id = "app-aimp";
    aimpApp.name = "aimp";
    aimpApp.target_class = "TAIMPMainForm";
    aimpApp.target_exe = "AIMP.exe";
    aimpApp.target_path = "C:\\Program Files (x86)\\AIMP\\AIMP.exe";
    aimpApp.auto_start = true;
    c.applications.push_back(aimpApp);

    CProfile aimp;
    aimp.id = "aimp";
    aimp.name = "aimp";
    aimp.mode = keysidekick::config::PROFILE_MODE_TARGETED;
    aimp.built_in = false;
    aimp.application_id = "app-aimp";
    CMapping m1; m1.usage_id = 0x1E; m1.modifiers = 0; m1.action = "1";
    CMapping m2; m2.usage_id = 0x1E; m2.modifiers = 0x33; m2.action = "!switch:basic";
    CMapping m3; m3.usage_id = 0x3A; m3.modifiers = 0; m3.action = "{F1}";
    aimp.mappings.push_back(m1);
    aimp.mappings.push_back(m2);
    aimp.mappings.push_back(m3);
    c.profiles.push_back(aimp);

    // basic profile
    CProfile basic;
    basic.id = "basic";
    basic.name = "basic";
    basic.mode = keysidekick::config::PROFILE_MODE_BASIC;
    basic.built_in = true;
    CMapping bm1; bm1.usage_id = 0x29; bm1.modifiers = 0; bm1.action = "!switch:aimp";
    CMapping bm2; bm2.usage_id = 0x1E; bm2.modifiers = 0x33; bm2.action = "!switch:aimp";
    basic.mappings.push_back(bm1);
    basic.mappings.push_back(bm2);
    c.profiles.push_back(basic);

    return c;
}

// Find a mapping by usage+mod in a config profile
static const CMapping* FindConfigMapping(const CProfile& p, int usage, unsigned char mod) {
    for (std::size_t i = 0; i < p.mappings.size(); ++i) {
        if (p.mappings[i].usage_id == usage && p.mappings[i].modifiers == mod)
            return &p.mappings[i];
    }
    return 0;
}

// Find a config profile by id
static const CProfile* FindConfigProfile(const Config& c, const std::string& id) {
    for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        if (c.profiles[i].id == id) return &c.profiles[i];
    }
    return 0;
}

static const Application* FindConfigApp(const Config& c, const std::string& id) {
    for (std::size_t i = 0; i < c.applications.size(); ++i) {
        if (c.applications[i].id == id) return &c.applications[i];
    }
    return 0;
}

static void TestBasicRoundTrip() {
    Config original = MakeAimpConfig();

    DomainModel domain = keysidekick::bridge::ConfigToDomain(original);

    // Should have 2 applications (aimp) — wait, only 1 app
    CHECK(domain.applications.size() == 1);
    CHECK(domain.applications[0].id() == "app-aimp");
    CHECK(domain.applications[0].windowClass == "TAIMPMainForm");
    CHECK(domain.applications[0].processName == "AIMP.exe");

    // Should have aimp + Normal profiles
    CHECK(domain.profiles.size() >= 2);

    // Active profile should resolve to Normal (basic → Normal)
    const keysidekick::Profile* normalProf = domain.findProfile(keysidekick::Profile::normalId());
    CHECK(normalProf != 0);
    CHECK(normalProf->mappings.size() == 2);  // basic mappings

    const keysidekick::Profile* aimpProf = domain.findProfile("aimp");
    CHECK(aimpProf != 0);
    CHECK(aimpProf->mode == ProfileMode::Targeted);
    CHECK(aimpProf->linkedApplicationIds.size() == 1);
    CHECK(aimpProf->linkedApplicationIds[0] == "app-aimp");
    CHECK(aimpProf->defaultApplicationId == "app-aimp");
    CHECK(aimpProf->mappings.size() == 3);

    // Back to config
    Config roundTripped = keysidekick::bridge::DomainToConfig(domain, original.general);

    const CProfile* aimp2 = FindConfigProfile(roundTripped, "aimp");
    CHECK(aimp2 != 0);
    CHECK(aimp2->application_id == "app-aimp");
    CHECK(aimp2->mappings.size() == 3);

    // Action strings must survive round-trip
    const CMapping* rt1 = FindConfigMapping(*aimp2, 0x1E, 0x33);
    CHECK(rt1 != 0);
    CHECK(rt1->action == "!switch:basic");

    const CMapping* rt2 = FindConfigMapping(*aimp2, 0x3A, 0);
    CHECK(rt2 != 0);
    CHECK(rt2->action == "{F1}");

    const Application* aimpApp2 = FindConfigApp(roundTripped, "app-aimp");
    CHECK(aimpApp2 != 0);
    CHECK(aimpApp2->target_class == "TAIMPMainForm");
}

static void TestModifierConversion() {
    // config 8-bit → domain 4-bit
    CHECK(keysidekick::bridge::ExpandConfigModifierToDomain(0x33) == (ModifierCtrl | ModifierShift));  // Ctrl+Shift
    CHECK(keysidekick::bridge::ExpandConfigModifierToDomain(0x11) == ModifierCtrl);  // LCtrl+RCtrl
    CHECK(keysidekick::bridge::ExpandConfigModifierToDomain(0x00) == ModifierNone);
    CHECK(keysidekick::bridge::ExpandConfigModifierToDomain(0x88) == ModifierMeta);  // LWin+RWin

    // domain 4-bit → config 8-bit
    CHECK(keysidekick::bridge::ExpandDomainModifierToConfig(ModifierCtrl | ModifierShift) == 0x33);
    CHECK(keysidekick::bridge::ExpandDomainModifierToConfig(ModifierAlt) == 0x44);
    CHECK(keysidekick::bridge::ExpandDomainModifierToConfig(ModifierNone) == 0x00);
}

static void TestParseLiveConfigIni() {
    // Parse the actual config.ini format through config_v3 → bridge
    const char* iniText =
        "[General]\n"
        "DeviceVIDPID=vid_1234&pid_abcd\n"
        "DefaultProfile=basic\n"
        "HTTPPort=8765\n"
        "HTTPEnabled=1\n"
        "TrayEnabled=1\n"
        "EnableLog=1\n"
        "\n"
        "[Profile.aimp]\n"
        "Mode=targeted\n"
        "TargetClass=TAIMPMainForm\n"
        "TargetExe=AIMP.exe\n"
        "TargetPath=C:\\Program Files (x86)\\AIMP\\AIMP.exe\n"
        "AutoStart=1\n"
        "\n"
        "[Profile.aimp.Keys]\n"
        "USAGE_1E=1\n"
        "USAGE_1E+Ctrl+Shift=!switch:basic\n"
        "USAGE_3A={F1}\n"
        "\n"
        "[Profile.basic]\n"
        "Mode=basic\n"
        "\n"
        "[Profile.basic.Keys]\n"
        "USAGE_29=!switch:aimp\n";

    keysidekick::config::ParseResult pr = Parse(iniText);
    CHECK(pr.ok());  // no ERROR diagnostics (warnings about migration are ok)

    DomainModel domain = keysidekick::bridge::ConfigToDomain(pr.config);

    // Migration should produce an application for the targeted aimp profile
    CHECK(domain.applications.size() >= 1);
    CHECK(domain.applications[0].windowClass == "TAIMPMainForm");

    // aimp profile should exist and be targeted
    const keysidekick::Profile* aimp = domain.findProfile("aimp");
    // Migration generates deterministic ids like "profile-aimp-<hash>"
    // So we search by name
    if (!aimp) {
        for (std::size_t i = 0; i < domain.profiles.size(); ++i) {
            if (domain.profiles[i].name == "aimp") {
                aimp = &domain.profiles[i];
                break;
            }
        }
    }
    CHECK(aimp != 0);
    CHECK(aimp->mode == ProfileMode::Targeted);
    CHECK(aimp->mappings.size() == 3);

    // Action strings preserved
    bool foundSwitch = false;
    for (std::size_t i = 0; i < aimp->mappings.size(); ++i) {
        if (aimp->mappings[i].action.profileId == "!switch:basic") {
            foundSwitch = true;
            break;
        }
    }
    CHECK(foundSwitch);
}

static void TestSerializeRoundTrip() {
    // Full: Config → Domain → Config → Serialize → Parse → Domain
    Config original = MakeAimpConfig();
    DomainModel domain1 = keysidekick::bridge::ConfigToDomain(original);
    Config config2 = keysidekick::bridge::DomainToConfig(domain1, original.general);

    keysidekick::config::SerializeResult sr = Serialize(config2);
    CHECK(sr.ok());
    CHECK(!sr.text.empty());

    // The serialized text should have [Application. sections
    CHECK(sr.text.find("[Application.") != std::string::npos);
    CHECK(sr.text.find("[Profile.") != std::string::npos);

    // Parse back
    keysidekick::config::ParseResult pr2 = Parse(sr.text);
    CHECK(pr2.ok());

    DomainModel domain2 = keysidekick::bridge::ConfigToDomain(pr2.config);

    // Application should survive
    CHECK(domain2.applications.size() == 1);
    CHECK(domain2.applications[0].windowClass == "TAIMPMainForm");
}

int main() {
    TestBasicRoundTrip();
    TestModifierConversion();
    TestParseLiveConfigIni();
    TestSerializeRoundTrip();

    if (g_failed == 0) {
        std::printf("All bridge tests passed (%d checks)\n", g_tests);
        return 0;
    }
    std::printf("%d/%d checks FAILED\n", g_failed, g_tests);
    return 1;
}
