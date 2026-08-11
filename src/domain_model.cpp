#include "domain_model.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace keysidekick {
namespace {

std::string lowerAscii(const std::string& value) {
    std::string result(value);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(result[index])));
    }
    return result;
}

bool isBlank(const std::string& value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!std::isspace(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool containsId(const std::vector<std::string>& ids, const std::string& id) {
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (equalsCaseInsensitive(ids[index], id)) {
            return true;
        }
    }
    return false;
}

void appendErrors(std::vector<ValidationError>& destination,
                  const std::vector<ValidationError>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

std::string indexedPath(const std::string& collection, std::size_t index) {
    std::ostringstream stream;
    stream << collection << '[' << index << ']';
    return stream.str();
}

void addError(std::vector<ValidationError>& errors,
              const std::string& path,
              const std::string& message) {
    errors.push_back(ValidationError(path, message));
}

bool hasOnlyKnownModifiers(unsigned int modifierMask) {
    const unsigned int knownModifiers = ModifierCtrl | ModifierShift |
                                        ModifierAlt | ModifierMeta;
    return (modifierMask & ~knownModifiers) == 0u;
}

void validateAction(const Action& action,
                    const DomainModel& model,
                    const std::string& path,
                    std::vector<ValidationError>& errors) {
    switch (action.type) {
        case ActionType::SendKey:
        case ActionType::SendMediaKey:
            if (action.usageId == 0u) {
                addError(errors, path + ".usageId", "usage ID must be non-zero");
            }
            break;
        case ActionType::SendShortcut:
            if (action.usageId == 0u) {
                addError(errors, path + ".usageId", "usage ID must be non-zero");
            }
            if (!hasOnlyKnownModifiers(action.modifierMask)) {
                addError(errors, path + ".modifierMask", "modifier mask contains unknown bits");
            }
            break;
        case ActionType::SwitchProfile:
            if (model.findProfile(action.profileId) == 0) {
                addError(errors, path + ".profileId", "referenced profile does not exist");
            }
            break;
        case ActionType::ToggleProfile:
            if (model.findProfile(action.profileId) == 0) {
                addError(errors, path + ".profileId", "referenced profile does not exist");
            }
            if (model.findProfile(action.secondaryProfileId) == 0) {
                addError(errors, path + ".secondaryProfileId", "referenced profile does not exist");
            }
            break;
        case ActionType::LaunchApplication:
            if (model.findApplication(action.applicationId) == 0) {
                addError(errors, path + ".applicationId", "referenced application does not exist");
            }
            break;
        case ActionType::SendToApplication:
            if (model.findApplication(action.applicationId) == 0) {
                addError(errors, path + ".applicationId", "referenced application does not exist");
            }
            if (action.steps.size() != 1u) {
                addError(errors, path + ".steps", "send-to-app requires exactly one nested action");
            } else {
                validateAction(action.steps[0], model, path + ".steps[0]", errors);
            }
            break;
        case ActionType::Sequence:
            if (action.steps.empty()) {
                addError(errors, path + ".steps", "sequence must contain at least one action");
            }
            for (std::size_t index = 0; index < action.steps.size(); ++index) {
                validateAction(action.steps[index], model,
                               indexedPath(path + ".steps", index), errors);
            }
            break;
    }
}

bool actionReferencesProfile(const Action& action,
                             const std::string& profileId) {
    if (action.type == ActionType::SwitchProfile &&
        equalsCaseInsensitive(action.profileId, profileId)) {
        return true;
    }
    if (action.type == ActionType::ToggleProfile &&
        (equalsCaseInsensitive(action.profileId, profileId) ||
         equalsCaseInsensitive(action.secondaryProfileId, profileId))) {
        return true;
    }
    for (std::size_t index = 0; index < action.steps.size(); ++index) {
        if (actionReferencesProfile(action.steps[index], profileId)) {
            return true;
        }
    }
    return false;
}

std::string fallbackProfileId(const DomainModel& model,
                              const std::string& excludedProfileId) {
    const Profile* normal = model.findProfile(Profile::normalId());
    if (normal != 0 &&
        !equalsCaseInsensitive(normal->id(), excludedProfileId)) {
        return normal->id();
    }
    for (std::size_t index = 0; index < model.profiles.size(); ++index) {
        if (!equalsCaseInsensitive(model.profiles[index].id(),
                                   excludedProfileId)) {
            return model.profiles[index].id();
        }
    }
    return std::string();
}

}  // namespace

ApplicationTarget::ApplicationTarget(const std::string& id,
                                     const std::string& name)
    : name(name),
      launchPolicy(LaunchPolicy::Never),
      windowPolicy(WindowPolicy::ExistingOnly),
      id_(id) {
}

const std::string& ApplicationTarget::id() const {
    return id_;
}

ActivationRule::ActivationRule()
    : enabled(true) {
}

ActivationRule::ActivationRule(const std::string& kind,
                               const std::string& value)
    : kind(kind), value(value), enabled(true) {
}

Trigger::Trigger()
    : usageId(0u), modifierMask(ModifierNone) {
}

Trigger::Trigger(unsigned int usageId, unsigned int modifierMask)
    : usageId(usageId), modifierMask(modifierMask) {
}

Action::Action(ActionType type)
    : type(type), usageId(0u), modifierMask(ModifierNone) {
}

Action Action::sendKey(unsigned int usageId) {
    Action action(ActionType::SendKey);
    action.usageId = usageId;
    return action;
}

Action Action::sendMediaKey(unsigned int usageId) {
    Action action(ActionType::SendMediaKey);
    action.usageId = usageId;
    return action;
}

Action Action::sendShortcut(unsigned int usageId,
                            unsigned int modifierMask) {
    Action action(ActionType::SendShortcut);
    action.usageId = usageId;
    action.modifierMask = modifierMask;
    return action;
}

Action Action::switchProfile(const std::string& profileId) {
    Action action(ActionType::SwitchProfile);
    action.profileId = profileId;
    return action;
}

Action Action::toggleProfile(const std::string& profileId,
                             const std::string& secondaryProfileId) {
    Action action(ActionType::ToggleProfile);
    action.profileId = profileId;
    action.secondaryProfileId = secondaryProfileId;
    return action;
}

Action Action::launchApplication(const std::string& applicationId) {
    Action action(ActionType::LaunchApplication);
    action.applicationId = applicationId;
    return action;
}

Action Action::sendToApplication(const std::string& applicationId,
                                 const Action& nestedAction) {
    Action action(ActionType::SendToApplication);
    action.applicationId = applicationId;
    action.steps.push_back(nestedAction);
    return action;
}

Action Action::sequence(const std::vector<Action>& steps) {
    Action action(ActionType::Sequence);
    action.steps = steps;
    return action;
}

Destination::Destination(DestinationType type)
    : type(type) {
}

Destination Destination::defaultApplication() {
    return Destination(DestinationType::DefaultApplication);
}

Destination Destination::applications(
    const std::vector<std::string>& applicationIds) {
    Destination destination(DestinationType::ApplicationIds);
    destination.applicationIds = applicationIds;
    return destination;
}

Destination Destination::allLinked() {
    return Destination(DestinationType::AllLinked);
}

Mapping::Mapping(const std::string& id,
                 int order,
                 const Trigger& trigger,
                 const Action& action,
                 const Destination& destination)
    : order(order),
      trigger(trigger),
      action(action),
      destination(destination),
      id_(id) {
}

const std::string& Mapping::id() const {
    return id_;
}

Profile::Profile(const std::string& id,
                 const std::string& name,
                 ProfileMode mode)
    : name(name), mode(mode), id_(id), builtIn_(false) {
}

Profile::Profile(const std::string& id,
                 const std::string& name,
                 ProfileMode mode,
                 bool builtIn)
    : name(name), mode(mode), id_(id), builtIn_(builtIn) {
}

const std::string& Profile::id() const {
    return id_;
}

bool Profile::isBuiltIn() const {
    return builtIn_;
}

const std::string& Profile::normalId() {
    static const std::string id("profile.normal");
    return id;
}

Profile Profile::normalProfile() {
    return Profile(normalId(), "Normal", ProfileMode::Normal, true);
}

DomainModel::DomainModel()
    : activeProfileId(Profile::normalId()) {
    profiles.push_back(Profile::normalProfile());
}

Profile* DomainModel::findProfile(const std::string& id) {
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (equalsCaseInsensitive(profiles[index].id(), id)) {
            return &profiles[index];
        }
    }
    return 0;
}

const Profile* DomainModel::findProfile(const std::string& id) const {
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (equalsCaseInsensitive(profiles[index].id(), id)) {
            return &profiles[index];
        }
    }
    return 0;
}

ApplicationTarget* DomainModel::findApplication(const std::string& id) {
    for (std::size_t index = 0; index < applications.size(); ++index) {
        if (equalsCaseInsensitive(applications[index].id(), id)) {
            return &applications[index];
        }
    }
    return 0;
}

const ApplicationTarget* DomainModel::findApplication(
    const std::string& id) const {
    for (std::size_t index = 0; index < applications.size(); ++index) {
        if (equalsCaseInsensitive(applications[index].id(), id)) {
            return &applications[index];
        }
    }
    return 0;
}

Profile& DomainModel::profileById(const std::string& id) {
    Profile* profile = findProfile(id);
    if (profile == 0) {
        throw NotFoundError("profile not found: " + id);
    }
    return *profile;
}

const Profile& DomainModel::profileById(const std::string& id) const {
    const Profile* profile = findProfile(id);
    if (profile == 0) {
        throw NotFoundError("profile not found: " + id);
    }
    return *profile;
}

ApplicationTarget& DomainModel::applicationById(const std::string& id) {
    ApplicationTarget* application = findApplication(id);
    if (application == 0) {
        throw NotFoundError("application not found: " + id);
    }
    return *application;
}

const ApplicationTarget& DomainModel::applicationById(
    const std::string& id) const {
    const ApplicationTarget* application = findApplication(id);
    if (application == 0) {
        throw NotFoundError("application not found: " + id);
    }
    return *application;
}

ValidationError::ValidationError(const std::string& path,
                                 const std::string& message)
    : path(path), message(message) {
}

bool equalsCaseInsensitive(const std::string& left,
                           const std::string& right) {
    return lowerAscii(left) == lowerAscii(right);
}

bool isValidStableId(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < id.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(id[index]);
        if (!std::isalnum(character) && character != '.' &&
            character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

std::vector<ValidationError> validateApplicationTarget(
    const ApplicationTarget& application) {
    std::vector<ValidationError> errors;
    if (!isValidStableId(application.id())) {
        addError(errors, "id", "application ID is empty or invalid");
    }
    if (isBlank(application.name)) {
        addError(errors, "name", "application name must not be blank");
    }
    if (application.launchPolicy != LaunchPolicy::Never &&
        isBlank(application.exePath)) {
        addError(errors, "exePath", "launchable application requires an executable path");
    }
    return errors;
}

std::vector<ValidationError> validateProfile(const Profile& profile,
                                             const DomainModel& model) {
    std::vector<ValidationError> errors;
    if (!isValidStableId(profile.id())) {
        addError(errors, "id", "profile ID is empty or invalid");
    }
    if (isBlank(profile.name)) {
        addError(errors, "name", "profile name must not be blank");
    }
    if (profile.isBuiltIn() &&
        (!equalsCaseInsensitive(profile.id(), Profile::normalId()) ||
         profile.name != "Normal" || profile.mode != ProfileMode::Normal)) {
        addError(errors, "builtIn", "built-in Normal profile has protected identity");
    }

    std::set<std::string> linkedIds;
    for (std::size_t index = 0;
         index < profile.linkedApplicationIds.size(); ++index) {
        const std::string normalized =
            lowerAscii(profile.linkedApplicationIds[index]);
        const std::string path = indexedPath("linkedApplicationIds", index);
        if (!linkedIds.insert(normalized).second) {
            addError(errors, path, "linked application ID is duplicated");
        }
        if (model.findApplication(profile.linkedApplicationIds[index]) == 0) {
            addError(errors, path, "linked application does not exist");
        }
    }
    if (!profile.defaultApplicationId.empty()) {
        if (!containsId(profile.linkedApplicationIds,
                        profile.defaultApplicationId)) {
            addError(errors, "defaultApplicationId", "default application must be linked");
        }
        if (model.findApplication(profile.defaultApplicationId) == 0) {
            addError(errors, "defaultApplicationId", "default application does not exist");
        }
    }

    std::set<std::string> mappingIds;
    std::set<int> mappingOrders;
    for (std::size_t mappingIndex = 0;
         mappingIndex < profile.mappings.size(); ++mappingIndex) {
        const Mapping& mapping = profile.mappings[mappingIndex];
        const std::string mappingPath =
            indexedPath("mappings", mappingIndex);
        if (!isValidStableId(mapping.id())) {
            addError(errors, mappingPath + ".id", "mapping ID is empty or invalid");
        }
        if (!mappingIds.insert(lowerAscii(mapping.id())).second) {
            addError(errors, mappingPath + ".id", "mapping ID is duplicated");
        }
        if (!mappingOrders.insert(mapping.order).second) {
            addError(errors, mappingPath + ".order", "mapping order is duplicated");
        }
        if (mapping.trigger.usageId == 0u) {
            addError(errors, mappingPath + ".trigger.usageId", "trigger usage ID must be non-zero");
        }
        if (!hasOnlyKnownModifiers(mapping.trigger.modifierMask)) {
            addError(errors, mappingPath + ".trigger.modifierMask", "modifier mask contains unknown bits");
        }
        if (mapping.destination.type == DestinationType::ApplicationIds) {
            if (mapping.destination.applicationIds.empty()) {
                addError(errors, mappingPath + ".destination.applicationIds", "explicit destination must not be empty");
            }
            std::set<std::string> destinationIds;
            for (std::size_t destinationIndex = 0;
                 destinationIndex < mapping.destination.applicationIds.size();
                 ++destinationIndex) {
                const std::string& applicationId =
                    mapping.destination.applicationIds[destinationIndex];
                const std::string destinationPath = indexedPath(
                    mappingPath + ".destination.applicationIds",
                    destinationIndex);
                if (!destinationIds.insert(lowerAscii(applicationId)).second) {
                    addError(errors, destinationPath, "destination application ID is duplicated");
                }
                if (!containsId(profile.linkedApplicationIds, applicationId)) {
                    addError(errors, destinationPath, "destination application must be linked");
                }
                if (model.findApplication(applicationId) == 0) {
                    addError(errors, destinationPath, "destination application does not exist");
                }
            }
        } else if (!mapping.destination.applicationIds.empty()) {
            addError(errors, mappingPath + ".destination.applicationIds", "destination type must not carry explicit application IDs");
        }
        validateAction(mapping.action, model, mappingPath + ".action", errors);
    }
    return errors;
}

std::vector<ValidationError> validateDomainModel(const DomainModel& model) {
    std::vector<ValidationError> errors;
    std::set<std::string> applicationIds;
    std::set<std::string> applicationNames;
    for (std::size_t index = 0; index < model.applications.size(); ++index) {
        const ApplicationTarget& application = model.applications[index];
        const std::string path = indexedPath("applications", index);
        std::vector<ValidationError> applicationErrors =
            validateApplicationTarget(application);
        for (std::size_t errorIndex = 0;
             errorIndex < applicationErrors.size(); ++errorIndex) {
            applicationErrors[errorIndex].path =
                path + "." + applicationErrors[errorIndex].path;
        }
        appendErrors(errors, applicationErrors);
        if (!applicationIds.insert(lowerAscii(application.id())).second) {
            addError(errors, path + ".id", "application ID must be unique case-insensitively");
        }
        if (!applicationNames.insert(lowerAscii(application.name)).second) {
            addError(errors, path + ".name", "application name must be unique case-insensitively");
        }
    }

    std::set<std::string> profileIds;
    std::set<std::string> profileNames;
    std::set<std::string> globalMappingIds;
    for (std::size_t index = 0; index < model.profiles.size(); ++index) {
        const Profile& profile = model.profiles[index];
        const std::string path = indexedPath("profiles", index);
        std::vector<ValidationError> profileErrors =
            validateProfile(profile, model);
        for (std::size_t errorIndex = 0;
             errorIndex < profileErrors.size(); ++errorIndex) {
            profileErrors[errorIndex].path =
                path + "." + profileErrors[errorIndex].path;
        }
        appendErrors(errors, profileErrors);
        if (!profileIds.insert(lowerAscii(profile.id())).second) {
            addError(errors, path + ".id", "profile ID must be unique case-insensitively");
        }
        if (!profileNames.insert(lowerAscii(profile.name)).second) {
            addError(errors, path + ".name", "profile name must be unique case-insensitively");
        }
        for (std::size_t mappingIndex = 0;
             mappingIndex < profile.mappings.size(); ++mappingIndex) {
            if (!globalMappingIds.insert(
                    lowerAscii(profile.mappings[mappingIndex].id())).second) {
                addError(errors,
                         indexedPath(path + ".mappings", mappingIndex) + ".id",
                         "mapping ID must be globally unique case-insensitively");
            }
        }
    }
    if (model.findProfile(Profile::normalId()) == 0) {
        addError(errors, "profiles", "built-in Normal profile is missing");
    }
    if (model.findProfile(model.activeProfileId) == 0) {
        addError(errors, "activeProfileId", "active profile does not exist");
    }
    return errors;
}

DomainError::DomainError(const std::string& message)
    : std::runtime_error(message) {
}

NotFoundError::NotFoundError(const std::string& message)
    : DomainError(message) {
}

DuplicateIdError::DuplicateIdError(const std::string& message)
    : DomainError(message) {
}

DuplicateNameError::DuplicateNameError(const std::string& message)
    : DomainError(message) {
}

BuiltInProfileError::BuiltInProfileError(const std::string& message)
    : DomainError(message) {
}

InvariantError::InvariantError(const std::string& message)
    : DomainError(message) {
}

ReferencedProfileError::ReferencedProfileError(const std::string& message)
    : DomainError(message) {
}

ProfileService::ProfileService(DomainModel& model)
    : model_(model) {
    if (model_.findProfile(model_.activeProfileId) == 0) {
        model_.activeProfileId = fallbackProfileId(model_, std::string());
    }
}

void ProfileService::checkNewProfileIdentity(const std::string& id,
                                             const std::string& name) const {
    if (!isValidStableId(id)) {
        throw InvariantError("profile ID is empty or invalid: " + id);
    }
    if (isBlank(name)) {
        throw InvariantError("profile name must not be blank");
    }
    if (model_.findProfile(id) != 0) {
        throw DuplicateIdError("profile ID already exists: " + id);
    }
    for (std::size_t index = 0; index < model_.profiles.size(); ++index) {
        if (equalsCaseInsensitive(model_.profiles[index].name, name)) {
            throw DuplicateNameError("profile name already exists: " + name);
        }
    }
}

Profile ProfileService::createProfile(const std::string& id,
                                      const std::string& name,
                                      ProfileMode mode) {
    checkNewProfileIdentity(id, name);
    Profile profile(id, name, mode);
    model_.profiles.push_back(profile);
    return profile;
}

std::string ProfileService::makeDuplicateMappingId(
    const std::string& profileId, std::size_t index) const {
    std::ostringstream base;
    base << profileId << ".mapping." << (index + 1u);
    std::string candidate = base.str();
    unsigned int suffix = 2u;
    bool collision = true;
    while (collision) {
        collision = false;
        for (std::size_t profileIndex = 0;
             profileIndex < model_.profiles.size() && !collision;
             ++profileIndex) {
            const Profile& profile = model_.profiles[profileIndex];
            for (std::size_t mappingIndex = 0;
                 mappingIndex < profile.mappings.size(); ++mappingIndex) {
                if (equalsCaseInsensitive(profile.mappings[mappingIndex].id(),
                                          candidate)) {
                    collision = true;
                    std::ostringstream withSuffix;
                    withSuffix << base.str() << '.' << suffix++;
                    candidate = withSuffix.str();
                    break;
                }
            }
        }
    }
    return candidate;
}

Profile ProfileService::duplicateProfile(
    const std::string& sourceProfileId,
    const std::string& newProfileId,
    const std::string& newName) {
    const Profile& source = model_.profileById(sourceProfileId);
    checkNewProfileIdentity(newProfileId, newName);

    Profile duplicate(newProfileId, newName, source.mode);
    duplicate.icon = source.icon;
    duplicate.linkedApplicationIds = source.linkedApplicationIds;
    duplicate.defaultApplicationId = source.defaultApplicationId;
    duplicate.activationRules = source.activationRules;
    for (std::size_t index = 0; index < source.mappings.size(); ++index) {
        const Mapping& sourceMapping = source.mappings[index];
        duplicate.mappings.push_back(Mapping(
            makeDuplicateMappingId(newProfileId, index),
            sourceMapping.order,
            sourceMapping.trigger,
            sourceMapping.action,
            sourceMapping.destination));
    }
    model_.profiles.push_back(duplicate);
    return duplicate;
}

void ProfileService::renameProfile(const std::string& profileId,
                                   const std::string& newName) {
    Profile& profile = model_.profileById(profileId);
    if (profile.isBuiltIn()) {
        throw BuiltInProfileError("built-in Normal profile cannot be renamed");
    }
    if (isBlank(newName)) {
        throw InvariantError("profile name must not be blank");
    }
    for (std::size_t index = 0; index < model_.profiles.size(); ++index) {
        const Profile& other = model_.profiles[index];
        if (!equalsCaseInsensitive(other.id(), profile.id()) &&
            equalsCaseInsensitive(other.name, newName)) {
            throw DuplicateNameError("profile name already exists: " + newName);
        }
    }
    profile.name = newName;
}

void ProfileService::deleteProfile(const std::string& profileId) {
    Profile& profile = model_.profileById(profileId);
    if (profile.isBuiltIn()) {
        throw BuiltInProfileError("built-in Normal profile cannot be deleted");
    }
    for (std::size_t profileIndex = 0;
         profileIndex < model_.profiles.size(); ++profileIndex) {
        const Profile& owner = model_.profiles[profileIndex];
        if (equalsCaseInsensitive(owner.id(), profile.id())) {
            continue;
        }
        for (std::size_t mappingIndex = 0;
             mappingIndex < owner.mappings.size(); ++mappingIndex) {
            if (actionReferencesProfile(owner.mappings[mappingIndex].action,
                                        profile.id())) {
                throw ReferencedProfileError(
                    "profile is referenced by mapping: " +
                    owner.mappings[mappingIndex].id());
            }
        }
    }

    const bool deletingActive =
        equalsCaseInsensitive(model_.activeProfileId, profile.id());
    const std::string fallback =
        deletingActive ? fallbackProfileId(model_, profile.id()) :
                         model_.activeProfileId;
    for (std::vector<Profile>::iterator iterator = model_.profiles.begin();
         iterator != model_.profiles.end(); ++iterator) {
        if (equalsCaseInsensitive(iterator->id(), profileId)) {
            model_.profiles.erase(iterator);
            break;
        }
    }
    if (deletingActive) {
        model_.activeProfileId = fallback;
    }
}

void ProfileService::activateProfile(const std::string& profileId) {
    model_.activeProfileId = model_.profileById(profileId).id();
}

void ProfileService::setDefaultApplication(
    const std::string& profileId,
    const std::string& applicationId) {
    Profile& profile = model_.profileById(profileId);
    if (applicationId.empty()) {
        profile.defaultApplicationId.clear();
        return;
    }
    model_.applicationById(applicationId);
    if (!containsId(profile.linkedApplicationIds, applicationId)) {
        throw InvariantError("default application must be linked: " +
                             applicationId);
    }
    profile.defaultApplicationId =
        model_.applicationById(applicationId).id();
}

void ProfileService::linkApplication(const std::string& profileId,
                                     const std::string& applicationId) {
    Profile& profile = model_.profileById(profileId);
    const ApplicationTarget& application =
        model_.applicationById(applicationId);
    if (!containsId(profile.linkedApplicationIds, application.id())) {
        profile.linkedApplicationIds.push_back(application.id());
    }
    if (profile.defaultApplicationId.empty()) {
        profile.defaultApplicationId = application.id();
    }
}

void ProfileService::unlinkApplication(const std::string& profileId,
                                       const std::string& applicationId) {
    Profile& profile = model_.profileById(profileId);
    for (std::vector<std::string>::iterator iterator =
             profile.linkedApplicationIds.begin();
         iterator != profile.linkedApplicationIds.end();) {
        if (equalsCaseInsensitive(*iterator, applicationId)) {
            iterator = profile.linkedApplicationIds.erase(iterator);
        } else {
            ++iterator;
        }
    }

    if (equalsCaseInsensitive(profile.defaultApplicationId, applicationId)) {
        profile.defaultApplicationId = profile.linkedApplicationIds.empty() ?
            std::string() : profile.linkedApplicationIds[0];
    }
    for (std::size_t mappingIndex = 0;
         mappingIndex < profile.mappings.size(); ++mappingIndex) {
        Destination& destination =
            profile.mappings[mappingIndex].destination;
        if (destination.type != DestinationType::ApplicationIds) {
            continue;
        }
        for (std::vector<std::string>::iterator iterator =
                 destination.applicationIds.begin();
             iterator != destination.applicationIds.end();) {
            if (equalsCaseInsensitive(*iterator, applicationId)) {
                iterator = destination.applicationIds.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (destination.applicationIds.empty()) {
            destination = Destination::defaultApplication();
        }
    }
}

}  // namespace keysidekick
