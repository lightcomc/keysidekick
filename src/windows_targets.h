#ifndef KEYSIDEKICK_WINDOWS_TARGETS_H
#define KEYSIDEKICK_WINDOWS_TARGETS_H

#include <cstdint>
#include <string>
#include <vector>

namespace keysidekick {
namespace windows_targets {

struct WindowCandidate {
    WindowCandidate();

    std::uintptr_t handle;
    std::wstring title;
    std::wstring windowClass;
    std::uint32_t processId;
    std::wstring processName;
    std::wstring processPath;
    bool processMetadataAvailable;
    bool visible;
    bool toolWindow;
    bool shellWindow;
};

struct WindowFilterPolicy {
    WindowFilterPolicy();

    bool requireVisible;
    bool excludeEmptyTitles;
    bool excludeToolWindows;
    bool excludeShellWindows;
};

struct TargetQuery {
    std::wstring windowClass;
    std::wstring processName;
    std::wstring processPath;
};

bool IsWindowCandidateAllowed(const WindowCandidate& candidate,
                              const WindowFilterPolicy& policy);

int ScoreTargetCandidate(const WindowCandidate& candidate,
                         const TargetQuery& query);

bool ResolveTarget(const std::vector<WindowCandidate>& candidates,
                   const TargetQuery& query,
                   WindowCandidate* resolved,
                   const WindowFilterPolicy& policy = WindowFilterPolicy());

std::vector<WindowCandidate> EnumerateWindows(
    const WindowFilterPolicy& policy = WindowFilterPolicy(),
    std::uint32_t* errorCode = NULL);

class WindowIcon {
public:
    WindowIcon();
    ~WindowIcon();
    WindowIcon(WindowIcon&& other) noexcept;
    WindowIcon& operator=(WindowIcon&& other) noexcept;

    WindowIcon(const WindowIcon&) = delete;
    WindowIcon& operator=(const WindowIcon&) = delete;

    bool valid() const;
    std::uintptr_t nativeHandle() const;
    int width() const;
    int height() const;
    std::uintptr_t release();

private:
    friend bool ExtractWindowIcon(std::uintptr_t windowHandle, WindowIcon* icon);

    void reset();

    std::uintptr_t handle_;
    int width_;
    int height_;
};

bool ExtractWindowIcon(std::uintptr_t windowHandle, WindowIcon* icon);

std::wstring BuildWindowsCommandLine(
    const std::wstring& applicationPath,
    const std::vector<std::wstring>& arguments);

class LaunchedProcess {
public:
    LaunchedProcess();
    ~LaunchedProcess();
    LaunchedProcess(LaunchedProcess&& other) noexcept;
    LaunchedProcess& operator=(LaunchedProcess&& other) noexcept;

    LaunchedProcess(const LaunchedProcess&) = delete;
    LaunchedProcess& operator=(const LaunchedProcess&) = delete;

    bool started() const;
    std::uint32_t processId() const;
    std::uint32_t errorCode() const;
    std::uintptr_t nativeProcessHandle() const;
    std::uintptr_t releaseProcessHandle();

private:
    friend LaunchedProcess LaunchApplication(
        const std::wstring& applicationPath,
        const std::vector<std::wstring>& arguments,
        const std::wstring& workingDirectory,
        std::uint32_t creationFlags);

    void reset();

    std::uintptr_t processHandle_;
    std::uint32_t processId_;
    std::uint32_t errorCode_;
};

LaunchedProcess LaunchApplication(
    const std::wstring& applicationPath,
    const std::vector<std::wstring>& arguments = std::vector<std::wstring>(),
    const std::wstring& workingDirectory = std::wstring(),
    std::uint32_t creationFlags = 0);

} // namespace windows_targets
} // namespace keysidekick

#endif
