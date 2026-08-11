#include "app_instance.h"

#include <cwctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace keysidekick {
namespace {

const DWORD kHashOffsetBasis = 2166136261u;
const DWORD kHashPrime = 16777619u;

DWORD HashWideString(const std::wstring& value) {
    DWORD hash = kHashOffsetBasis;
    for (std::wstring::const_iterator character = value.begin();
         character != value.end();
         ++character) {
        const unsigned int codeUnit = static_cast<unsigned int>(*character);
        hash ^= codeUnit & 0xffu;
        hash *= kHashPrime;
        hash ^= (codeUnit >> 8) & 0xffu;
        hash *= kHashPrime;
    }
    return hash;
}

std::wstring HashToken(const std::wstring& value) {
    std::wostringstream token;
    token << std::hex << std::nouppercase << std::setfill(L'0')
          << std::setw(8) << HashWideString(value);
    return token.str();
}

bool IsSafeNameCharacter(wchar_t character) {
    return (character >= L'a' && character <= L'z') ||
           (character >= L'A' && character <= L'Z') ||
           (character >= L'0' && character <= L'9') ||
           character == L'.' || character == L'-' || character == L'_';
}

std::wstring ReadableNameToken(const std::wstring& value) {
    std::wstring token;
    token.reserve(value.size());
    for (std::wstring::const_iterator character = value.begin();
         character != value.end();
         ++character) {
        token.push_back(IsSafeNameCharacter(*character) ? *character : L'_');
    }
    if (token.size() > 48) token.resize(48);
    return token;
}

std::wstring Lowercase(const std::wstring& value) {
    std::wstring lowercase;
    lowercase.reserve(value.size());
    for (std::wstring::const_iterator character = value.begin();
         character != value.end();
         ++character) {
        lowercase.push_back(static_cast<wchar_t>(std::towlower(*character)));
    }
    return lowercase;
}

AppInstanceAcquireResult AcquireResult(AppInstanceAcquireStatus status,
                                       DWORD win32Error) {
    AppInstanceAcquireResult result;
    result.status = status;
    result.win32_error = win32Error;
    return result;
}

} // namespace

AppLaunchMode ParseAppLaunchMode(const std::vector<std::wstring>& arguments) {
    AppLaunchMode mode = AppLaunchMode::Default;
    for (std::vector<std::wstring>::const_iterator argument = arguments.begin();
         argument != arguments.end();
         ++argument) {
        const std::wstring normalized = Lowercase(*argument);
        if (normalized == L"--agent") {
            mode = AppLaunchMode::Agent;
        } else if (normalized == L"--supervise") {
            mode = AppLaunchMode::Supervise;
        } else if (normalized == L"--open-dashboard") {
            mode = AppLaunchMode::OpenDashboard;
        } else if (normalized == L"--diagnostics") {
            mode = AppLaunchMode::Diagnostics;
        }
    }
    return mode;
}

AppInstanceNames BuildAppInstanceNames(const std::wstring& stable_app_name,
                                       const std::wstring& scope_suffix) {
    AppInstanceNames names;
    if (stable_app_name.empty()) return names;

    const std::wstring identity = stable_app_name + L"\x001f" + scope_suffix;
    const std::wstring readable = ReadableNameToken(stable_app_name);
    const std::wstring base = L"KeySidekick." + readable + L"." +
                              HashToken(identity);
    names.mutex_name = L"Local\\" + base + L".Instance";
    names.open_event_name = L"Local\\" + base + L".OpenDashboard";
    names.window_message_name = base + L".OpenDashboard.Message";
    return names;
}

AppInstance::AppInstance(const std::wstring& stable_app_name,
                         const std::wstring& scope_suffix)
    : names_(BuildAppInstanceNames(stable_app_name, scope_suffix)),
      mutex_handle_(NULL),
      open_event_handle_(NULL),
      window_message_(0),
      initialization_error_(ERROR_SUCCESS),
      owns_instance_(false) {
    if (stable_app_name.empty()) {
        initialization_error_ = ERROR_INVALID_PARAMETER;
        return;
    }

    window_message_ = RegisterWindowMessageW(names_.window_message_name.c_str());
    if (window_message_ == 0) initialization_error_ = GetLastError();
}

AppInstance::~AppInstance() {
    CloseHandles();
}

AppInstance::AppInstance(AppInstance&& other) noexcept
    : names_(std::move(other.names_)),
      mutex_handle_(other.mutex_handle_),
      open_event_handle_(other.open_event_handle_),
      window_message_(other.window_message_),
      initialization_error_(other.initialization_error_),
      owns_instance_(other.owns_instance_) {
    other.mutex_handle_ = NULL;
    other.open_event_handle_ = NULL;
    other.window_message_ = 0;
    other.initialization_error_ = ERROR_SUCCESS;
    other.owns_instance_ = false;
}

AppInstance& AppInstance::operator=(AppInstance&& other) noexcept {
    if (this == &other) return *this;

    CloseHandles();
    names_ = std::move(other.names_);
    mutex_handle_ = other.mutex_handle_;
    open_event_handle_ = other.open_event_handle_;
    window_message_ = other.window_message_;
    initialization_error_ = other.initialization_error_;
    owns_instance_ = other.owns_instance_;

    other.mutex_handle_ = NULL;
    other.open_event_handle_ = NULL;
    other.window_message_ = 0;
    other.initialization_error_ = ERROR_SUCCESS;
    other.owns_instance_ = false;
    return *this;
}

AppInstanceAcquireResult AppInstance::Acquire() {
    if (owns_instance_) {
        return AcquireResult(AppInstanceAcquireStatus::Acquired, ERROR_SUCCESS);
    }
    if (initialization_error_ != ERROR_SUCCESS) {
        return AcquireResult(AppInstanceAcquireStatus::Error,
                             initialization_error_);
    }
    if (mutex_handle_ != NULL) {
        return AcquireResult(AppInstanceAcquireStatus::AlreadyRunning,
                             ERROR_SUCCESS);
    }

    mutex_handle_ = CreateMutexW(NULL, FALSE, names_.mutex_name.c_str());
    if (mutex_handle_ == NULL) {
        return AcquireResult(AppInstanceAcquireStatus::Error, GetLastError());
    }

    const DWORD mutexError = GetLastError();
    if (mutexError == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_handle_);
        mutex_handle_ = NULL;
        return AcquireResult(AppInstanceAcquireStatus::AlreadyRunning,
                             ERROR_SUCCESS);
    }

    open_event_handle_ = CreateEventW(NULL, FALSE, FALSE,
                                      names_.open_event_name.c_str());
    if (open_event_handle_ == NULL) {
        const DWORD eventError = GetLastError();
        CloseHandle(mutex_handle_);
        mutex_handle_ = NULL;
        return AcquireResult(AppInstanceAcquireStatus::Error, eventError);
    }

    owns_instance_ = true;
    return AcquireResult(AppInstanceAcquireStatus::Acquired, ERROR_SUCCESS);
}

bool AppInstance::RequestOpenDashboard() {
    if (initialization_error_ != ERROR_SUCCESS) return false;

    HANDLE eventHandle =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, names_.open_event_name.c_str());
    if (eventHandle == NULL) return false;

    const BOOL signaled = SetEvent(eventHandle);
    CloseHandle(eventHandle);
    if (signaled && window_message_ != 0) {
        PostMessageW(HWND_BROADCAST, window_message_, 0, 0);
    }
    return signaled != FALSE;
}

bool AppInstance::ConsumeOpenDashboardRequest() {
    if (!owns_instance_ || open_event_handle_ == NULL) return false;
    return WaitForSingleObject(open_event_handle_, 0) == WAIT_OBJECT_0;
}

bool AppInstance::owns_instance() const {
    return owns_instance_;
}

UINT AppInstance::window_message() const {
    return window_message_;
}

const AppInstanceNames& AppInstance::names() const {
    return names_;
}

void AppInstance::CloseHandles() {
    if (open_event_handle_ != NULL) {
        CloseHandle(open_event_handle_);
        open_event_handle_ = NULL;
    }
    if (mutex_handle_ != NULL) {
        CloseHandle(mutex_handle_);
        mutex_handle_ = NULL;
    }
    owns_instance_ = false;
}

} // namespace keysidekick
