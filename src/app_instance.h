#ifndef KEYSIDEKICK_APP_INSTANCE_H
#define KEYSIDEKICK_APP_INSTANCE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace keysidekick {

enum class AppLaunchMode {
    Default,
    Agent,
    Supervise,
    OpenDashboard,
    Diagnostics
};

AppLaunchMode ParseAppLaunchMode(const std::vector<std::wstring>& arguments);

struct AppInstanceNames {
    std::wstring mutex_name;
    std::wstring open_event_name;
    std::wstring window_message_name;
};

AppInstanceNames BuildAppInstanceNames(const std::wstring& stable_app_name,
                                       const std::wstring& scope_suffix);

enum class AppInstanceAcquireStatus {
    Acquired,
    AlreadyRunning,
    Error
};

struct AppInstanceAcquireResult {
    AppInstanceAcquireStatus status;
    DWORD win32_error;
};

class AppInstance {
public:
    explicit AppInstance(const std::wstring& stable_app_name,
                         const std::wstring& scope_suffix = L"");
    ~AppInstance();

    AppInstance(AppInstance&& other) noexcept;
    AppInstance& operator=(AppInstance&& other) noexcept;

    AppInstance(const AppInstance&) = delete;
    AppInstance& operator=(const AppInstance&) = delete;

    AppInstanceAcquireResult Acquire();
    bool RequestOpenDashboard();
    bool ConsumeOpenDashboardRequest();

    bool owns_instance() const;
    UINT window_message() const;
    const AppInstanceNames& names() const;

private:
    void CloseHandles();

    AppInstanceNames names_;
    HANDLE mutex_handle_;
    HANDLE open_event_handle_;
    UINT window_message_;
    DWORD initialization_error_;
    bool owns_instance_;
};

} // namespace keysidekick

#endif
