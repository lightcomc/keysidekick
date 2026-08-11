#ifndef KEYSIDEKICK_DOMAIN_MODEL_H
#define KEYSIDEKICK_DOMAIN_MODEL_H

#include <stdexcept>
#include <string>
#include <vector>

namespace keysidekick {

enum class LaunchPolicy {
    Never,
    IfNotRunning,
    Always
};

enum class WindowPolicy {
    ExistingOnly,
    ExistingOrLaunch,
    ForegroundPreferred
};

enum class ProfileMode {
    Normal,
    Targeted
};

enum Modifier {
    ModifierNone = 0,
    ModifierCtrl = 1 << 0,
    ModifierShift = 1 << 1,
    ModifierAlt = 1 << 2,
    ModifierMeta = 1 << 3
};

class ApplicationTarget {
public:
    ApplicationTarget(const std::string& id, const std::string& name);

    const std::string& id() const;

    std::string name;
    std::string icon;
    std::string exePath;
    std::string processName;
    std::string windowClass;
    LaunchPolicy launchPolicy;
    WindowPolicy windowPolicy;

private:
    std::string id_;
};

struct ActivationRule {
    ActivationRule();
    ActivationRule(const std::string& kind, const std::string& value);

    std::string kind;
    std::string value;
    bool enabled;
};

struct Trigger {
    Trigger();
    Trigger(unsigned int usageId, unsigned int modifierMask);

    unsigned int usageId;
    unsigned int modifierMask;
};

enum class ActionType {
    SendKey,
    SendMediaKey,
    SendShortcut,
    SwitchProfile,
    ToggleProfile,
    LaunchApplication,
    SendToApplication,
    Sequence
};

class Action {
public:
    static Action sendKey(unsigned int usageId);
    static Action sendMediaKey(unsigned int usageId);
    static Action sendShortcut(unsigned int usageId,
                               unsigned int modifierMask);
    static Action switchProfile(const std::string& profileId);
    static Action toggleProfile(const std::string& profileId,
                                const std::string& secondaryProfileId);
    static Action launchApplication(const std::string& applicationId);
    static Action sendToApplication(const std::string& applicationId,
                                    const Action& action);
    static Action sequence(const std::vector<Action>& steps);

    ActionType type;
    unsigned int usageId;
    unsigned int modifierMask;
    std::string profileId;
    std::string secondaryProfileId;
    std::string applicationId;
    std::vector<Action> steps;

private:
    explicit Action(ActionType type);
};

enum class DestinationType {
    DefaultApplication,
    ApplicationIds,
    AllLinked
};

struct Destination {
    static Destination defaultApplication();
    static Destination applications(
        const std::vector<std::string>& applicationIds);
    static Destination allLinked();

    DestinationType type;
    std::vector<std::string> applicationIds;

private:
    explicit Destination(DestinationType type);
};

class Mapping {
public:
    Mapping(const std::string& id,
            int order,
            const Trigger& trigger,
            const Action& action,
            const Destination& destination);

    const std::string& id() const;

    int order;
    Trigger trigger;
    Action action;
    Destination destination;

private:
    std::string id_;
};

class Profile {
public:
    Profile(const std::string& id,
            const std::string& name,
            ProfileMode mode);

    const std::string& id() const;
    bool isBuiltIn() const;

    static const std::string& normalId();
    static Profile normalProfile();

    std::string name;
    std::string icon;
    ProfileMode mode;
    std::string layerModifier;   // Fn-слой: имя модификатора ("Alt"/"Ctrl"/...), "" = выкл
    std::vector<std::string> linkedApplicationIds;
    std::string defaultApplicationId;
    std::vector<ActivationRule> activationRules;
    std::vector<Mapping> mappings;

private:
    Profile(const std::string& id,
            const std::string& name,
            ProfileMode mode,
            bool builtIn);

    std::string id_;
    bool builtIn_;
};

class DomainModel {
public:
    DomainModel();

    Profile* findProfile(const std::string& id);
    const Profile* findProfile(const std::string& id) const;
    ApplicationTarget* findApplication(const std::string& id);
    const ApplicationTarget* findApplication(const std::string& id) const;

    Profile& profileById(const std::string& id);
    const Profile& profileById(const std::string& id) const;
    ApplicationTarget& applicationById(const std::string& id);
    const ApplicationTarget& applicationById(const std::string& id) const;

    std::vector<ApplicationTarget> applications;
    std::vector<Profile> profiles;
    std::string activeProfileId;
};

struct ValidationError {
    ValidationError(const std::string& path, const std::string& message);

    std::string path;
    std::string message;
};

bool equalsCaseInsensitive(const std::string& left,
                           const std::string& right);
bool isValidStableId(const std::string& id);
std::vector<ValidationError> validateApplicationTarget(
    const ApplicationTarget& application);
std::vector<ValidationError> validateProfile(const Profile& profile,
                                             const DomainModel& model);
std::vector<ValidationError> validateDomainModel(const DomainModel& model);

class DomainError : public std::runtime_error {
public:
    explicit DomainError(const std::string& message);
};

class NotFoundError : public DomainError {
public:
    explicit NotFoundError(const std::string& message);
};

class DuplicateIdError : public DomainError {
public:
    explicit DuplicateIdError(const std::string& message);
};

class DuplicateNameError : public DomainError {
public:
    explicit DuplicateNameError(const std::string& message);
};

class BuiltInProfileError : public DomainError {
public:
    explicit BuiltInProfileError(const std::string& message);
};

class InvariantError : public DomainError {
public:
    explicit InvariantError(const std::string& message);
};

class ReferencedProfileError : public DomainError {
public:
    explicit ReferencedProfileError(const std::string& message);
};

class ProfileService {
public:
    explicit ProfileService(DomainModel& model);

    Profile createProfile(const std::string& id,
                          const std::string& name,
                          ProfileMode mode);
    Profile duplicateProfile(const std::string& sourceProfileId,
                             const std::string& newProfileId,
                             const std::string& newName);
    void renameProfile(const std::string& profileId,
                       const std::string& newName);
    void deleteProfile(const std::string& profileId);
    void activateProfile(const std::string& profileId);
    void setDefaultApplication(const std::string& profileId,
                               const std::string& applicationId);
    void linkApplication(const std::string& profileId,
                         const std::string& applicationId);
    void unlinkApplication(const std::string& profileId,
                           const std::string& applicationId);

private:
    DomainModel& model_;

    void checkNewProfileIdentity(const std::string& id,
                                 const std::string& name) const;
    std::string makeDuplicateMappingId(const std::string& profileId,
                                       std::size_t index) const;
};

}  // namespace keysidekick

#endif
