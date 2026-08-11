#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "windows_targets.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>
#include <vector>

namespace keysidekick {
namespace windows_targets {
namespace {

const int kNoMatch = -1;
const int kClassScore = 100;
const int kProcessNameScore = 20;
const int kProcessPathScore = 40;

std::wstring Lowercase(const std::wstring& value) {
    std::wstring lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return lowered;
}

bool EqualsCaseInsensitive(const std::wstring& left,
                           const std::wstring& right) {
    return Lowercase(left) == Lowercase(right);
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const std::wstring::size_type separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return path;
    return path.substr(separator + 1);
}

bool ReadWindowText(HWND window, std::wstring* text) {
    if (text == NULL) return false;
    text->clear();

    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return false;

    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, &buffer[0], static_cast<int>(buffer.size()));
    if (copied <= 0) return false;

    text->assign(&buffer[0], static_cast<std::size_t>(copied));
    return true;
}

bool ReadWindowClass(HWND window, std::wstring* windowClass) {
    if (windowClass == NULL) return false;
    windowClass->clear();

    std::vector<wchar_t> buffer(256, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const int copied = GetClassNameW(window, &buffer[0], static_cast<int>(buffer.size()));
        if (copied <= 0) return false;
        if (static_cast<std::size_t>(copied) + 1 < buffer.size()) {
            windowClass->assign(&buffer[0], static_cast<std::size_t>(copied));
            return true;
        }
        if (buffer.size() >= 32768) return false;
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

bool ReadProcessPath(std::uint32_t processId, std::wstring* path) {
    if (path == NULL || processId == 0) return false;
    path->clear();

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(processId));
    if (process == NULL) return false;

    std::vector<wchar_t> buffer(512, L'\0');
    bool succeeded = false;
    for (;;) {
        DWORD size = static_cast<DWORD>(buffer.size());
        SetLastError(ERROR_SUCCESS);
        if (QueryFullProcessImageNameW(process, 0, &buffer[0], &size)) {
            path->assign(&buffer[0], static_cast<std::size_t>(size));
            succeeded = true;
            break;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
            break;
        }
        buffer.resize(buffer.size() * 2, L'\0');
    }

    CloseHandle(process);
    return succeeded;
}

struct EnumerationContext {
    EnumerationContext(const WindowFilterPolicy& requestedPolicy,
                       std::vector<WindowCandidate>* outputCandidates)
        : policy(requestedPolicy), candidates(outputCandidates) {}

    WindowFilterPolicy policy;
    std::vector<WindowCandidate>* candidates;
};

BOOL CALLBACK EnumerateWindowCallback(HWND window, LPARAM parameter) {
    EnumerationContext* context = reinterpret_cast<EnumerationContext*>(parameter);
    if (context == NULL || context->candidates == NULL) return FALSE;

    WindowCandidate candidate;
    candidate.handle = reinterpret_cast<std::uintptr_t>(window);
    candidate.visible = IsWindowVisible(window) != FALSE;
    candidate.toolWindow =
        (static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_EXSTYLE)) & WS_EX_TOOLWINDOW) != 0;
    candidate.shellWindow = window == GetShellWindow();

    ReadWindowText(window, &candidate.title);
    ReadWindowClass(window, &candidate.windowClass);

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    candidate.processId = static_cast<std::uint32_t>(processId);
    candidate.processMetadataAvailable = ReadProcessPath(candidate.processId, &candidate.processPath);
    if (candidate.processMetadataAvailable) {
        candidate.processName = FileNameFromPath(candidate.processPath);
    }

    if (IsWindowCandidateAllowed(candidate, context->policy)) {
        context->candidates->push_back(candidate);
    }
    return TRUE;
}

bool CandidatePrecedes(const WindowCandidate& left,
                       const WindowCandidate& right) {
    if (left.handle != right.handle) return left.handle < right.handle;
    if (left.processId != right.processId) return left.processId < right.processId;

    const std::wstring leftClass = Lowercase(left.windowClass);
    const std::wstring rightClass = Lowercase(right.windowClass);
    if (leftClass != rightClass) return leftClass < rightClass;

    const std::wstring leftPath = Lowercase(left.processPath);
    const std::wstring rightPath = Lowercase(right.processPath);
    if (leftPath != rightPath) return leftPath < rightPath;

    return Lowercase(left.title) < Lowercase(right.title);
}

bool ArgumentNeedsQuotes(const std::wstring& argument) {
    if (argument.empty()) return true;
    return argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument,
                                  bool alwaysQuote) {
    if (!alwaysQuote && !ArgumentNeedsQuotes(argument)) return argument;

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashCount = 0;

    for (std::wstring::const_iterator it = argument.begin(); it != argument.end(); ++it) {
        if (*it == L'\\') {
            ++backslashCount;
            continue;
        }

        if (*it == L'"') {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashCount, L'\\');
            quoted.push_back(*it);
        }
        backslashCount = 0;
    }

    quoted.append(backslashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool ReadIconDimensions(HICON icon, int* width, int* height) {
    if (icon == NULL || width == NULL || height == NULL) return false;

    ICONINFO iconInfo;
    ZeroMemory(&iconInfo, sizeof(iconInfo));
    if (!GetIconInfo(icon, &iconInfo)) return false;

    BITMAP bitmap;
    ZeroMemory(&bitmap, sizeof(bitmap));
    bool succeeded = false;
    if (iconInfo.hbmColor != NULL &&
        GetObjectW(iconInfo.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
        *width = bitmap.bmWidth;
        *height = bitmap.bmHeight;
        succeeded = true;
    } else if (iconInfo.hbmMask != NULL &&
               GetObjectW(iconInfo.hbmMask, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
        *width = bitmap.bmWidth;
        *height = bitmap.bmHeight / 2;
        succeeded = true;
    }

    if (iconInfo.hbmColor != NULL) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask != NULL) DeleteObject(iconInfo.hbmMask);
    return succeeded;
}

} // namespace

WindowCandidate::WindowCandidate()
    : handle(0),
      processId(0),
      processMetadataAvailable(false),
      visible(false),
      toolWindow(false),
      shellWindow(false) {}

WindowFilterPolicy::WindowFilterPolicy()
    : requireVisible(true),
      excludeEmptyTitles(true),
      excludeToolWindows(true),
      excludeShellWindows(true) {}

bool IsWindowCandidateAllowed(const WindowCandidate& candidate,
                              const WindowFilterPolicy& policy) {
    if (policy.requireVisible && !candidate.visible) return false;
    if (policy.excludeEmptyTitles && candidate.title.empty()) return false;
    if (policy.excludeToolWindows && candidate.toolWindow) return false;
    if (policy.excludeShellWindows && candidate.shellWindow) return false;
    return true;
}

int ScoreTargetCandidate(const WindowCandidate& candidate,
                         const TargetQuery& query) {
    if (query.windowClass.empty() || candidate.windowClass.empty()) return kNoMatch;
    if (!EqualsCaseInsensitive(candidate.windowClass, query.windowClass)) return kNoMatch;

    int score = kClassScore;

    if (!query.processName.empty()) {
        std::wstring candidateProcessName = candidate.processName;
        if (candidateProcessName.empty()) {
            candidateProcessName = FileNameFromPath(candidate.processPath);
        }
        if (candidateProcessName.empty() ||
            !EqualsCaseInsensitive(candidateProcessName, query.processName)) {
            return kNoMatch;
        }
        score += kProcessNameScore;
    }

    if (!query.processPath.empty()) {
        if (candidate.processPath.empty() ||
            !EqualsCaseInsensitive(candidate.processPath, query.processPath)) {
            return kNoMatch;
        }
        score += kProcessPathScore;
    }

    return score;
}

bool ResolveTarget(const std::vector<WindowCandidate>& candidates,
                   const TargetQuery& query,
                   WindowCandidate* resolved,
                   const WindowFilterPolicy& policy) {
    if (resolved == NULL || query.windowClass.empty()) return false;

    const WindowCandidate* best = NULL;
    int bestScore = kNoMatch;

    for (std::vector<WindowCandidate>::const_iterator it = candidates.begin();
         it != candidates.end(); ++it) {
        if (!IsWindowCandidateAllowed(*it, policy)) continue;

        const int score = ScoreTargetCandidate(*it, query);
        if (score < 0) continue;
        if (best == NULL || score > bestScore ||
            (score == bestScore && CandidatePrecedes(*it, *best))) {
            best = &*it;
            bestScore = score;
        }
    }

    if (best == NULL) return false;
    *resolved = *best;
    return true;
}

std::vector<WindowCandidate> EnumerateWindows(const WindowFilterPolicy& policy,
                                               std::uint32_t* errorCode) {
    if (errorCode != NULL) *errorCode = ERROR_SUCCESS;

    std::vector<WindowCandidate> candidates;
    EnumerationContext context(policy, &candidates);
    SetLastError(ERROR_SUCCESS);
    if (!EnumWindows(EnumerateWindowCallback, reinterpret_cast<LPARAM>(&context))) {
        const DWORD error = GetLastError();
        if (errorCode != NULL) {
            *errorCode = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        }
    }

    std::sort(candidates.begin(), candidates.end(), CandidatePrecedes);
    return candidates;
}

WindowIcon::WindowIcon() : handle_(0), width_(0), height_(0) {}

WindowIcon::~WindowIcon() {
    reset();
}

WindowIcon::WindowIcon(WindowIcon&& other) noexcept
    : handle_(other.handle_), width_(other.width_), height_(other.height_) {
    other.handle_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

WindowIcon& WindowIcon::operator=(WindowIcon&& other) noexcept {
    if (this == &other) return *this;
    reset();
    handle_ = other.handle_;
    width_ = other.width_;
    height_ = other.height_;
    other.handle_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

bool WindowIcon::valid() const {
    return handle_ != 0;
}

std::uintptr_t WindowIcon::nativeHandle() const {
    return handle_;
}

int WindowIcon::width() const {
    return width_;
}

int WindowIcon::height() const {
    return height_;
}

std::uintptr_t WindowIcon::release() {
    const std::uintptr_t released = handle_;
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    return released;
}

void WindowIcon::reset() {
    if (handle_ != 0) {
        DestroyIcon(reinterpret_cast<HICON>(handle_));
        handle_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

bool ExtractWindowIcon(std::uintptr_t windowHandle, WindowIcon* icon) {
    if (windowHandle == 0 || icon == NULL) return false;
    icon->reset();

    const HWND window = reinterpret_cast<HWND>(windowHandle);
    if (!IsWindow(window)) return false;

    DWORD_PTR messageResult = 0;
    HICON borrowed = NULL;
    if (SendMessageTimeoutW(window, WM_GETICON, ICON_SMALL2, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                            &messageResult)) {
        borrowed = reinterpret_cast<HICON>(messageResult);
    }
    if (borrowed == NULL &&
        SendMessageTimeoutW(window, WM_GETICON, ICON_SMALL, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                            &messageResult)) {
        borrowed = reinterpret_cast<HICON>(messageResult);
    }
    if (borrowed == NULL) {
        borrowed = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    }
    if (borrowed == NULL) {
        borrowed = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    }
    if (borrowed == NULL) return false;

    const HICON owned = CopyIcon(borrowed);
    if (owned == NULL) return false;

    int width = 0;
    int height = 0;
    if (!ReadIconDimensions(owned, &width, &height)) {
        DestroyIcon(owned);
        return false;
    }

    icon->handle_ = reinterpret_cast<std::uintptr_t>(owned);
    icon->width_ = width;
    icon->height_ = height;
    return true;
}

std::wstring BuildWindowsCommandLine(
    const std::wstring& applicationPath,
    const std::vector<std::wstring>& arguments) {
    if (applicationPath.empty()) return std::wstring();

    std::wstring commandLine = QuoteWindowsArgument(applicationPath, true);
    for (std::vector<std::wstring>::const_iterator it = arguments.begin();
         it != arguments.end(); ++it) {
        commandLine.push_back(L' ');
        commandLine.append(QuoteWindowsArgument(*it, false));
    }
    return commandLine;
}

LaunchedProcess::LaunchedProcess()
    : processHandle_(0), processId_(0), errorCode_(ERROR_SUCCESS) {}

LaunchedProcess::~LaunchedProcess() {
    reset();
}

LaunchedProcess::LaunchedProcess(LaunchedProcess&& other) noexcept
    : processHandle_(other.processHandle_),
      processId_(other.processId_),
      errorCode_(other.errorCode_) {
    other.processHandle_ = 0;
    other.processId_ = 0;
    other.errorCode_ = ERROR_SUCCESS;
}

LaunchedProcess& LaunchedProcess::operator=(LaunchedProcess&& other) noexcept {
    if (this == &other) return *this;
    reset();
    processHandle_ = other.processHandle_;
    processId_ = other.processId_;
    errorCode_ = other.errorCode_;
    other.processHandle_ = 0;
    other.processId_ = 0;
    other.errorCode_ = ERROR_SUCCESS;
    return *this;
}

bool LaunchedProcess::started() const {
    return processHandle_ != 0;
}

std::uint32_t LaunchedProcess::processId() const {
    return processId_;
}

std::uint32_t LaunchedProcess::errorCode() const {
    return errorCode_;
}

std::uintptr_t LaunchedProcess::nativeProcessHandle() const {
    return processHandle_;
}

std::uintptr_t LaunchedProcess::releaseProcessHandle() {
    const std::uintptr_t released = processHandle_;
    processHandle_ = 0;
    return released;
}

void LaunchedProcess::reset() {
    if (processHandle_ != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(processHandle_));
        processHandle_ = 0;
    }
    processId_ = 0;
}

LaunchedProcess LaunchApplication(
    const std::wstring& applicationPath,
    const std::vector<std::wstring>& arguments,
    const std::wstring& workingDirectory,
    std::uint32_t creationFlags) {
    LaunchedProcess result;
    if (applicationPath.empty()) {
        result.errorCode_ = ERROR_INVALID_PARAMETER;
        return result;
    }

    std::wstring commandLine = BuildWindowsCommandLine(applicationPath, arguments);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInformation;
    ZeroMemory(&processInformation, sizeof(processInformation));

    const wchar_t* workingDirectoryPointer =
        workingDirectory.empty() ? NULL : workingDirectory.c_str();
    if (!CreateProcessW(applicationPath.c_str(), &mutableCommandLine[0],
                        NULL, NULL, FALSE, static_cast<DWORD>(creationFlags),
                        NULL, workingDirectoryPointer, &startupInfo,
                        &processInformation)) {
        result.errorCode_ = GetLastError();
        return result;
    }

    CloseHandle(processInformation.hThread);
    result.processHandle_ = reinterpret_cast<std::uintptr_t>(processInformation.hProcess);
    result.processId_ = static_cast<std::uint32_t>(processInformation.dwProcessId);
    result.errorCode_ = ERROR_SUCCESS;
    return result;
}

} // namespace windows_targets
} // namespace keysidekick
