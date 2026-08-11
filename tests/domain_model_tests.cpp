#include "../src/domain_model.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace keysidekick;

int failures = 0;

void fail(const char* testName, const std::string& message) {
    ++failures;
    std::cerr << "FAIL " << testName << ": " << message << std::endl;
}

#define CHECK(TEST_NAME, CONDITION) \
    do { \
        if (!(CONDITION)) { \
            fail(TEST_NAME, "check failed: " #CONDITION); \
            return; \
        } \
    } while (false)

template <typename Exception, typename Function>
void checkThrows(const char* testName, Function function) {
    try {
        function();
        fail(testName, "expected exception was not thrown");
    } catch (const Exception&) {
    } catch (...) {
        fail(testName, "unexpected exception type");
    }
}

ApplicationTarget makeApplication(const std::string& id,
                                  const std::string& name) {
    ApplicationTarget application(id, name);
    application.icon = name + ".png";
    application.exePath = "C:/Apps/" + name + ".exe";
    application.processName = name + ".exe";
    application.windowClass = name + "Window";
    application.launchPolicy = LaunchPolicy::IfNotRunning;
    application.windowPolicy = WindowPolicy::ExistingOrLaunch;
    return application;
}

Mapping makeMapping(const std::string& id,
                    int order,
                    const Destination& destination,
                    const Action& action) {
    return Mapping(id, order, Trigger(0x3A, ModifierCtrl | ModifierShift),
                   action, destination);
}

void multiAppProfileSupportsTypedActions() {
    const char* testName = "multi-app profile supports typed actions";
    DomainModel model;
    model.applications.push_back(makeApplication("app.music", "Music"));
    model.applications.push_back(makeApplication("app.chat", "Chat"));

    ProfileService service(model);
    Profile profile = service.createProfile("profile.stream", "Streaming",
                                            ProfileMode::Targeted);
    service.linkApplication(profile.id(), "app.music");
    service.linkApplication(profile.id(), "app.chat");
    service.setDefaultApplication(profile.id(), "app.music");

    Profile& stored = model.profileById(profile.id());
    stored.icon = "streaming.png";
    stored.activationRules.push_back(ActivationRule("future-rule", "placeholder"));
    stored.mappings.push_back(makeMapping(
        "mapping.play", 10, Destination::defaultApplication(),
        Action::sendMediaKey(0x00CD)));
    stored.mappings.push_back(makeMapping(
        "mapping.shortcut", 20, Destination::applications(
            std::vector<std::string>(1, "app.chat")),
        Action::sendShortcut(0x06, ModifierCtrl)));
    std::vector<Action> steps;
    steps.push_back(Action::sendKey(0x04));
    steps.push_back(Action::switchProfile(Profile::normalId()));
    stored.mappings.push_back(makeMapping(
        "mapping.sequence", 30, Destination::allLinked(),
        Action::sequence(steps)));

    CHECK(testName, stored.linkedApplicationIds.size() == 2u);
    CHECK(testName, stored.defaultApplicationId == "app.music");
    CHECK(testName, stored.activationRules.size() == 1u);
    CHECK(testName, stored.mappings[0].action.type == ActionType::SendMediaKey);
    CHECK(testName, stored.mappings[0].action.usageId == 0x00CD);
    CHECK(testName, stored.mappings[1].action.type == ActionType::SendShortcut);
    CHECK(testName, stored.mappings[1].action.modifierMask == ModifierCtrl);
    CHECK(testName, stored.mappings[2].action.type == ActionType::Sequence);
    CHECK(testName, stored.mappings[2].action.steps.size() == 2u);
    CHECK(testName, validateDomainModel(model).empty());
}

void perMappingDestinationsSupportOneMultipleAndAll() {
    const char* testName = "per-mapping destinations support one multiple and all";
    Destination one = Destination::applications(
        std::vector<std::string>(1, "app.music"));
    std::vector<std::string> multipleIds;
    multipleIds.push_back("app.music");
    multipleIds.push_back("app.chat");
    Destination multiple = Destination::applications(multipleIds);
    Destination all = Destination::allLinked();

    CHECK(testName, one.type == DestinationType::ApplicationIds);
    CHECK(testName, one.applicationIds.size() == 1u);
    CHECK(testName, multiple.applicationIds.size() == 2u);
    CHECK(testName, all.type == DestinationType::AllLinked);
    CHECK(testName, all.applicationIds.empty());
}

void duplicateIsIndependentAndGetsFreshStableIds() {
    const char* testName = "duplicate is independent and gets fresh stable ids";
    DomainModel model;
    model.applications.push_back(makeApplication("app.music", "Music"));
    ProfileService service(model);
    service.createProfile("profile.source", "Source", ProfileMode::Targeted);
    service.linkApplication("profile.source", "app.music");
    service.setDefaultApplication("profile.source", "app.music");
    model.profileById("profile.source").mappings.push_back(makeMapping(
        "mapping.source", 1, Destination::defaultApplication(),
        Action::launchApplication("app.music")));

    Profile copy = service.duplicateProfile(
        "profile.source", "profile.copy", "Copy");
    Profile& storedCopy = model.profileById(copy.id());
    CHECK(testName, storedCopy.id() == "profile.copy");
    CHECK(testName, storedCopy.name == "Copy");
    CHECK(testName, storedCopy.mappings.size() == 1u);
    CHECK(testName, storedCopy.mappings[0].id() !=
                        model.profileById("profile.source").mappings[0].id());

    storedCopy.linkedApplicationIds.clear();
    storedCopy.defaultApplicationId.clear();
    storedCopy.mappings[0].action = Action::sendKey(0x05);
    const Profile& source = model.profileById("profile.source");
    CHECK(testName, source.linkedApplicationIds.size() == 1u);
    CHECK(testName, source.defaultApplicationId == "app.music");
    CHECK(testName, source.mappings[0].action.type == ActionType::LaunchApplication);
}

void renameKeepsIdAndProfileReferencesStable() {
    const char* testName = "rename keeps id and profile references stable";
    DomainModel model;
    ProfileService service(model);
    service.createProfile("profile.alpha", "Alpha", ProfileMode::Targeted);
    service.createProfile("profile.control", "Control", ProfileMode::Normal);
    Profile& control = model.profileById("profile.control");
    control.mappings.push_back(makeMapping(
        "mapping.switch", 1, Destination::defaultApplication(),
        Action::switchProfile("profile.alpha")));
    control.mappings.push_back(makeMapping(
        "mapping.toggle", 2, Destination::defaultApplication(),
        Action::toggleProfile("profile.alpha", Profile::normalId())));

    service.renameProfile("profile.alpha", "Studio");

    CHECK(testName, model.profileById("profile.alpha").name == "Studio");
    CHECK(testName, control.mappings[0].action.profileId == "profile.alpha");
    CHECK(testName, control.mappings[1].action.profileId == "profile.alpha");
    CHECK(testName, control.mappings[1].action.secondaryProfileId == Profile::normalId());
    checkThrows<DuplicateNameError>(testName, [&service]() {
        service.createProfile("profile.other", "studio", ProfileMode::Normal);
    });
}

void deleteActiveFallsBackAndRejectsIncomingReferences() {
    const char* testName = "delete active falls back and rejects incoming references";
    DomainModel model;
    ProfileService service(model);
    service.createProfile("profile.active", "Active", ProfileMode::Targeted);
    service.createProfile("profile.control", "Control", ProfileMode::Normal);
    service.activateProfile("profile.active");

    Profile& control = model.profileById("profile.control");
    control.mappings.push_back(makeMapping(
        "mapping.switch", 1, Destination::defaultApplication(),
        Action::switchProfile("profile.active")));
    checkThrows<ReferencedProfileError>(testName, [&service]() {
        service.deleteProfile("profile.active");
    });
    CHECK(testName, model.activeProfileId == "profile.active");

    control.mappings.clear();
    service.deleteProfile("profile.active");
    CHECK(testName, model.activeProfileId == Profile::normalId());
    CHECK(testName, model.findProfile("profile.active") == 0);
}

void builtInNormalIsProtected() {
    const char* testName = "built-in normal is protected";
    DomainModel model;
    ProfileService service(model);

    checkThrows<BuiltInProfileError>(testName, [&service]() {
        service.renameProfile(Profile::normalId(), "Renamed");
    });
    checkThrows<BuiltInProfileError>(testName, [&service]() {
        service.deleteProfile(Profile::normalId());
    });
    CHECK(testName, model.profileById(Profile::normalId()).name == "Normal");
}

void linkAndUnlinkMaintainDefaultsAndDestinations() {
    const char* testName = "link and unlink maintain defaults and destinations";
    DomainModel model;
    model.applications.push_back(makeApplication("app.music", "Music"));
    model.applications.push_back(makeApplication("app.chat", "Chat"));
    ProfileService service(model);
    service.createProfile("profile.stream", "Stream", ProfileMode::Targeted);
    service.linkApplication("profile.stream", "app.music");
    service.linkApplication("profile.stream", "app.chat");
    service.setDefaultApplication("profile.stream", "app.music");

    Profile& profile = model.profileById("profile.stream");
    std::vector<std::string> explicitIds;
    explicitIds.push_back("app.music");
    explicitIds.push_back("app.chat");
    profile.mappings.push_back(makeMapping(
        "mapping.explicit", 1, Destination::applications(explicitIds),
        Action::sendKey(0x04)));
    profile.mappings.push_back(makeMapping(
        "mapping.all", 2, Destination::allLinked(), Action::sendKey(0x05)));

    service.unlinkApplication("profile.stream", "app.music");
    CHECK(testName, profile.defaultApplicationId == "app.chat");
    CHECK(testName, profile.mappings[0].destination.applicationIds.size() == 1u);
    CHECK(testName, profile.mappings[0].destination.applicationIds[0] == "app.chat");
    CHECK(testName, profile.mappings[1].destination.type == DestinationType::AllLinked);

    service.unlinkApplication("profile.stream", "app.chat");
    CHECK(testName, profile.defaultApplicationId.empty());
    CHECK(testName, profile.mappings[0].destination.type == DestinationType::DefaultApplication);
    CHECK(testName, profile.mappings[0].destination.applicationIds.empty());
    checkThrows<InvariantError>(testName, [&service]() {
        service.setDefaultApplication("profile.stream", "app.music");
    });
}

void validationReportsDuplicateIdsAndInvalidReferences() {
    const char* testName = "validation reports duplicate ids and invalid references";
    DomainModel model;
    model.applications.push_back(makeApplication("app.music", "Music"));
    model.applications.push_back(makeApplication("APP.MUSIC", "Other"));
    model.profiles.push_back(Profile::normalProfile());
    Profile broken("profile.broken", "Broken", ProfileMode::Targeted);
    broken.linkedApplicationIds.push_back("app.missing");
    broken.defaultApplicationId = "app.missing";
    broken.mappings.push_back(makeMapping(
        "mapping.bad", 1,
        Destination::applications(std::vector<std::string>(1, "app.missing")),
        Action::switchProfile("profile.missing")));
    model.profiles.push_back(broken);
    model.activeProfileId = "profile.missing";

    std::vector<ValidationError> errors = validateDomainModel(model);
    CHECK(testName, errors.size() >= 5u);
}

void actionFactoriesCoverDomainVariants() {
    const char* testName = "action factories cover domain variants";
    CHECK(testName, Action::sendKey(0x04).type == ActionType::SendKey);
    CHECK(testName, Action::sendMediaKey(0x00E9).type == ActionType::SendMediaKey);
    CHECK(testName, Action::sendShortcut(0x04, ModifierAlt).type == ActionType::SendShortcut);
    CHECK(testName, Action::switchProfile("profile.x").type == ActionType::SwitchProfile);
    CHECK(testName, Action::toggleProfile("profile.x", "profile.y").type == ActionType::ToggleProfile);
    CHECK(testName, Action::launchApplication("app.x").type == ActionType::LaunchApplication);
    CHECK(testName, Action::sendToApplication("app.x", Action::sendKey(0x04)).type == ActionType::SendToApplication);
    std::vector<Action> steps(1, Action::sendKey(0x04));
    CHECK(testName, Action::sequence(steps).type == ActionType::Sequence);
}

}  // namespace

int main() {
    multiAppProfileSupportsTypedActions();
    perMappingDestinationsSupportOneMultipleAndAll();
    duplicateIsIndependentAndGetsFreshStableIds();
    renameKeepsIdAndProfileReferencesStable();
    deleteActiveFallsBackAndRejectsIncomingReferences();
    builtInNormalIsProtected();
    linkAndUnlinkMaintainDefaultsAndDestinations();
    validationReportsDuplicateIdsAndInvalidReferences();
    actionFactoriesCoverDomainVariants();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "All domain model tests passed" << std::endl;
    return EXIT_SUCCESS;
}
