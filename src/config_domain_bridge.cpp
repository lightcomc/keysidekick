#include "config_domain_bridge.h"

#include <cstdio>
#include <set>
#include <sstream>

namespace keysidekick {
namespace bridge {

namespace {

// ---- Modifier bit constants (config 8-bit L/R layout) ----
const unsigned char CFG_LCTRL  = 0x01;
const unsigned char CFG_RCTRL  = 0x10;
const unsigned char CFG_LSHIFT = 0x02;
const unsigned char CFG_RSHIFT = 0x20;
const unsigned char CFG_LALT   = 0x04;
const unsigned char CFG_RALT   = 0x40;
const unsigned char CFG_LWIN   = 0x08;
const unsigned char CFG_RWIN   = 0x80;

// Build a deterministic mapping id for a config mapping (which has no id).
// Format: "<profileId>.<usageHex>.<configModHex>" — stable across round-trips
// as long as profile id and key spec don't change.
std::string MakeMappingId(const std::string& profileId,
                          int usageId,
                          unsigned char configMod) {
    std::ostringstream s;
    s << profileId << "." << std::hex << (unsigned)(usageId & 0xFF) << "."
      << (unsigned)configMod;
    return s.str();
}

// Build a deterministic application id from config fields.
// Config Application already has an id; this is only used if id is empty.
std::string FallbackAppId(const std::string& name) {
    if (!name.empty()) return name;
    return "app.unnamed";
}

} // namespace

unsigned char ExpandConfigModifierToDomain(unsigned char configMask) {
    unsigned char domain = 0;
    if (configMask & (CFG_LCTRL | CFG_RCTRL))  domain |= ModifierCtrl;
    if (configMask & (CFG_LSHIFT | CFG_RSHIFT)) domain |= ModifierShift;
    if (configMask & (CFG_LALT | CFG_RALT))    domain |= ModifierAlt;
    if (configMask & (CFG_LWIN | CFG_RWIN))    domain |= ModifierMeta;
    return domain;
}

unsigned char ExpandDomainModifierToConfig(unsigned int domainMask) {
    unsigned char config = 0;
    if (domainMask & ModifierCtrl)  config |= (CFG_LCTRL | CFG_RCTRL);
    if (domainMask & ModifierShift) config |= (CFG_LSHIFT | CFG_RSHIFT);
    if (domainMask & ModifierAlt)   config |= (CFG_LALT | CFG_RALT);
    if (domainMask & ModifierMeta)  config |= (CFG_LWIN | CFG_RWIN);
    return config;
}

DomainModel ConfigToDomain(const config::Config& config) {
    DomainModel model;

    // Clear the auto-seeded Normal profile if config has its own basic profile.
    // We'll re-add Normal only if config doesn't have one.
    bool hasNormalProfile = false;

    // ---- Applications ----
    model.applications.clear();
    for (std::size_t i = 0; i < config.applications.size(); ++i) {
        const config::Application& src = config.applications[i];
        std::string appId = src.id.empty() ? FallbackAppId(src.name) : src.id;
        ApplicationTarget app(appId, src.name.empty() ? src.id : src.name);
        app.windowClass  = src.target_class;
        app.exePath      = src.target_path;
        app.processName  = src.target_exe;
        // launchPolicy/windowPolicy: config has no concept — leave defaults (Never/ExistingOnly)
        // auto_start/default_profile_id: config-only concepts, not carried into domain
        model.applications.push_back(app);
    }

    // ---- Profiles ----
    model.profiles.clear();

    for (std::size_t i = 0; i < config.profiles.size(); ++i) {
        const config::Profile& src = config.profiles[i];
        ProfileMode mode = (src.mode == config::PROFILE_MODE_TARGETED)
                         ? ProfileMode::Targeted
                         : ProfileMode::Normal;

        // Detect built-in Normal profile: built_in flag OR id/name match
        bool isNormal = src.built_in ||
                        src.id == Profile::normalId() ||
                        equalsCaseInsensitive(src.name, "Normal") ||
                        equalsCaseInsensitive(src.name, "basic") ||
                        equalsCaseInsensitive(src.id, "basic");

        if (isNormal && !hasNormalProfile) {
            // Use the canonical built-in Normal profile
            model.profiles.push_back(Profile::normalProfile());
            hasNormalProfile = true;
        }

        Profile* domainProf = 0;
        if (isNormal) {
            // Find the Normal profile we just added and populate mappings into it
            for (std::size_t p = 0; p < model.profiles.size(); ++p) {
                if (model.profiles[p].id() == Profile::normalId()) {
                    domainProf = &model.profiles[p];
                    break;
                }
            }
        } else {
            std::string profId = src.id.empty() ? src.name : src.id;
            model.profiles.push_back(Profile(profId, src.name, mode));
            domainProf = &model.profiles.back();
        }

        if (!domainProf) continue;

        domainProf->layerModifier = src.layer_modifier;

        // Link application
        if (!src.application_id.empty()) {
            const ApplicationTarget* app = model.findApplication(src.application_id);
            if (app) {
                domainProf->linkedApplicationIds.push_back(app->id());
                domainProf->defaultApplicationId = app->id();
            }
        }

        // Mappings (pass-through action strings)
        for (std::size_t m = 0; m < src.mappings.size(); ++m) {
            const config::Mapping& srcMap = src.mappings[m];
            unsigned int usageId = (unsigned int)(srcMap.usage_id & 0xFF);
            unsigned char domainMod = ExpandConfigModifierToDomain(srcMap.modifiers);
            Trigger trigger(usageId, domainMod);

            // Pass-through: carry the action string in Action::profileId.
            // Action type = SendKey with usageId = trigger key so validation passes
            // (validateAction requires usageId != 0 for SendKey).
            Action action = Action::sendKey(usageId);
            action.profileId = srcMap.action;  // documented pass-through carrier

            Destination dest = Destination::defaultApplication();
            std::string mapId = MakeMappingId(domainProf->id(),
                                               srcMap.usage_id, srcMap.modifiers);
            domainProf->mappings.push_back(
                Mapping(mapId, (int)m, trigger, action, dest));
        }
    }

    // Ensure Normal profile exists (if config didn't have one)
    if (!hasNormalProfile) {
        model.profiles.insert(model.profiles.begin(), Profile::normalProfile());
    }

    // ---- Active profile ----
    model.activeProfileId = config.general.default_profile_id;
    if (model.activeProfileId.empty()) {
        model.activeProfileId = Profile::normalId();
    }

    // Validate active profile exists; fallback to Normal
    if (!model.findProfile(model.activeProfileId)) {
        // Try case-insensitive match by name ("basic" → Normal)
        for (std::size_t p = 0; p < model.profiles.size(); ++p) {
            if (equalsCaseInsensitive(model.profiles[p].name,
                                      config.general.default_profile_id)) {
                model.activeProfileId = model.profiles[p].id();
                break;
            }
        }
    }
    if (!model.findProfile(model.activeProfileId)) {
        model.activeProfileId = Profile::normalId();
    }

    return model;
}

config::Config DomainToConfig(const DomainModel& model,
                              const config::GeneralSettings& general) {
    config::Config config;
    config.schema_version = 3;
    config.general = general;
    // If general.default_profile_id references the domain Normal profile,
    // remap to canonical config id "basic".
    if (config.general.default_profile_id == Profile::normalId()) {
        config.general.default_profile_id = "basic";
    }

    // ---- Applications ----
    for (std::size_t i = 0; i < model.applications.size(); ++i) {
        const ApplicationTarget& app = model.applications[i];
        config::Application out;
        out.id    = app.id();
        out.name  = app.name.empty() ? app.id() : app.name;
        out.target_class = app.windowClass;
        out.target_exe   = app.processName;
        out.target_path  = app.exePath;
        // auto_start / default_profile_id: domain has no concept — leave defaults
        config.applications.push_back(out);
    }

    // ---- Profiles ----
    for (std::size_t i = 0; i < model.profiles.size(); ++i) {
        const Profile& prof = model.profiles[i];
        config::Profile out;
        // Domain Normal profile has id "profile.normal" (with a dot) which config_v3
        // rejects (only alnum/-/_ allowed). Map it to canonical config id "basic".
        if (prof.id() == Profile::normalId()) {
            out.id = "basic";
            out.name = "basic";
        } else {
            out.id = prof.id();
            out.name = prof.name;
        }
        out.mode = (prof.mode == ProfileMode::Targeted)
                 ? config::PROFILE_MODE_TARGETED
                 : config::PROFILE_MODE_BASIC;
        out.built_in = prof.isBuiltIn();
        out.layer_modifier = prof.layerModifier;

        // Lossy: take default application (or first linked) as the single application_id
        if (!prof.defaultApplicationId.empty()) {
            out.application_id = prof.defaultApplicationId;
        } else if (!prof.linkedApplicationIds.empty()) {
            out.application_id = prof.linkedApplicationIds.front();
        }

        // Mappings (pass-through action strings)
        for (std::size_t m = 0; m < prof.mappings.size(); ++m) {
            const Mapping& map = prof.mappings[m];
            config::Mapping outMap;
            outMap.usage_id = (int)(map.trigger.usageId & 0xFF);
            outMap.modifiers = ExpandDomainModifierToConfig(map.trigger.modifierMask);
            outMap.action = map.action.profileId;  // read pass-through carrier
            out.mappings.push_back(outMap);
        }
        config.profiles.push_back(out);
    }

    return config;
}

} // namespace bridge
} // namespace keysidekick
