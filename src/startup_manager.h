#pragma once

#include <optional>
#include <string>

namespace keysidekick {

inline constexpr wchar_t kStartupTaskName[] = L"KeySidekick";
inline constexpr wchar_t kLegacyShortcutName[] = L"KeySidekick.lnk";

enum class StartupError {
    None,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    AccessDenied,
    ComInitializationFailed,
    TaskSchedulerUnavailable,
    QueryFailed,
    RegistrationFailed,
    RemovalFailed,
    FilesystemError
};

enum class StartupAction {
    None,
    Inspected,
    Created,
    Updated,
    Removed,
    Unchanged,
    Preview
};

enum class DesiredStateComparison {
    Equivalent,
    Different
};

enum class StartupManagerMode {
    System,
    TestOnly
};

struct StartupOptions {
    std::wstring executable_path;
    std::wstring arguments;
    std::wstring working_directory;
    std::wstring user_id;
};

struct StartupStatus {
    StartupError error = StartupError::None;
    StartupAction action = StartupAction::None;
    bool task_exists = false;
    bool desired_state = false;
    bool changed = false;
    long native_error = 0;
    std::wstring message;
    std::optional<std::wstring> task_xml;

    bool ok() const noexcept { return error == StartupError::None; }
};

struct LegacyStartupStatus {
    StartupError error = StartupError::None;
    bool exists = false;
    std::wstring shortcut_path;
    std::wstring message;

    bool ok() const noexcept { return error == StartupError::None; }
};

StartupStatus BuildTaskXml(const StartupOptions& options);
DesiredStateComparison CompareTaskXml(const std::wstring& existing_xml,
                                      const std::wstring& desired_xml);
LegacyStartupStatus DetectLegacyStartupShortcut(
    const std::wstring& startup_directory);
LegacyStartupStatus DetectLegacyStartupShortcut();

class StartupManager {
public:
    explicit StartupManager(
        StartupManagerMode mode = StartupManagerMode::System) noexcept;

    StartupStatus Inspect(const StartupOptions& desired) const;
    StartupStatus Create(const StartupOptions& desired) const;
    StartupStatus Update(const StartupOptions& desired) const;
    StartupStatus Remove() const;
    StartupStatus Ensure(const StartupOptions& desired) const;

private:
    StartupManagerMode mode_;
};

}  // namespace keysidekick
