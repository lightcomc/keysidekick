#include "startup_manager.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <taskschd.h>

#include <cwctype>
#include <sstream>
#include <utility>
#include <vector>

namespace keysidekick {
namespace {

constexpr wchar_t kTaskNamespace[] =
    L"http://schemas.microsoft.com/windows/2004/02/mit/task";

class ScopedComInitialization {
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          owns_initialization_(SUCCEEDED(result_)) {}

    ~ScopedComInitialization() {
        if (owns_initialization_) {
            CoUninitialize();
        }
    }

    HRESULT result() const noexcept { return result_; }
    bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
    bool owns_initialization_;
};

template <typename Interface>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    Interface* Get() const noexcept { return value_; }
    Interface** Put() noexcept {
        Reset();
        return &value_;
    }
    Interface* operator->() const noexcept { return value_; }

    void Reset() noexcept {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    Interface* value_ = nullptr;
};

class ScopedBstr {
public:
    explicit ScopedBstr(const std::wstring& value)
        : value_(SysAllocStringLen(value.data(),
                                   static_cast<UINT>(value.size()))) {}

    ~ScopedBstr() { SysFreeString(value_); }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    BSTR Get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

StartupStatus Failure(StartupError error,
                      StartupAction action,
                      HRESULT native_error,
                      std::wstring message) {
    StartupStatus status;
    status.error = error;
    status.action = action;
    status.native_error = static_cast<long>(native_error);
    status.message = std::move(message);
    return status;
}

StartupError MapSchedulerError(HRESULT result, StartupError fallback) {
    if (result == E_ACCESSDENIED ||
        result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        return StartupError::AccessDenied;
    }
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return StartupError::NotFound;
    }
    return fallback;
}

std::wstring HexError(HRESULT result) {
    std::wostringstream output;
    output << L"HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return output.str();
}

bool IsValidXmlText(const std::wstring& value) {
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t current = value[index];
        if (current == L'\t' || current == L'\n' || current == L'\r') {
            continue;
        }
        if (current < 0x20 || current == 0xFFFE || current == 0xFFFF) {
            return false;
        }
        if (current >= 0xD800 && current <= 0xDBFF) {
            if (index + 1 >= value.size() || value[index + 1] < 0xDC00 ||
                value[index + 1] > 0xDFFF) {
                return false;
            }
            ++index;
        } else if (current >= 0xDC00 && current <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

std::wstring EscapeXml(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t current : value) {
        switch (current) {
            case L'&':
                escaped += L"&amp;";
                break;
            case L'<':
                escaped += L"&lt;";
                break;
            case L'>':
                escaped += L"&gt;";
                break;
            case L'\"':
                escaped += L"&quot;";
                break;
            case L'\'':
                escaped += L"&apos;";
                break;
            default:
                escaped += current;
                break;
        }
    }
    return escaped;
}

bool IsAbsoluteWindowsPath(const std::wstring& path) {
    if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 3 &&
           ((path[0] == L'\\' && path[1] == L'\\') ||
            (path[0] == L'/' && path[1] == L'/'));
}

bool ContainsInvalidWindowsPathCharacter(const std::wstring& path) {
    for (size_t index = 0; index < path.size(); ++index) {
        const wchar_t current = path[index];
        if (current < 0x20 || current == L'\"' || current == L'<' ||
            current == L'>' || current == L'|') {
            return true;
        }
        if ((current == L':' || current == L'*' || current == L'?') &&
            !(current == L':' && index == 1 && std::iswalpha(path[0]))) {
            return true;
        }
    }
    return false;
}

std::wstring NormalizeSeparators(std::wstring path) {
    for (wchar_t& current : path) {
        if (current == L'/') {
            current = L'\\';
        }
    }
    return path;
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return std::wstring();
    }
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}

std::wstring JoinWindowsPath(const std::wstring& directory,
                             const std::wstring& name) {
    if (directory.empty()) {
        return name;
    }
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') {
        return directory + name;
    }
    return directory + L"\\" + name;
}

std::optional<std::wstring> CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::nullopt;
    }

    DWORD required_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required_size);
    if (required_size == 0) {
        CloseHandle(token);
        return std::nullopt;
    }

    std::vector<unsigned char> token_data(required_size);
    if (!GetTokenInformation(token, TokenUser, token_data.data(), required_size,
                             &required_size)) {
        CloseHandle(token);
        return std::nullopt;
    }
    CloseHandle(token);

    const TOKEN_USER* token_user =
        reinterpret_cast<const TOKEN_USER*>(token_data.data());
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
        return std::nullopt;
    }

    std::wstring result(sid_text);
    LocalFree(sid_text);
    return result;
}

StartupStatus NormalizeOptions(const StartupOptions& requested,
                               StartupOptions* normalized) {
    *normalized = requested;

    if (!IsAbsoluteWindowsPath(normalized->executable_path) ||
        ContainsInvalidWindowsPathCharacter(normalized->executable_path)) {
        return Failure(StartupError::InvalidArgument, StartupAction::Preview,
                       E_INVALIDARG,
                       L"The executable path must be a valid absolute Windows path.");
    }

    normalized->executable_path =
        NormalizeSeparators(normalized->executable_path);
    if (normalized->working_directory.empty()) {
        normalized->working_directory = ParentDirectory(normalized->executable_path);
    }
    if (!IsAbsoluteWindowsPath(normalized->working_directory) ||
        ContainsInvalidWindowsPathCharacter(normalized->working_directory)) {
        return Failure(StartupError::InvalidArgument, StartupAction::Preview,
                       E_INVALIDARG,
                       L"The working directory must be a valid absolute Windows path.");
    }
    normalized->working_directory =
        NormalizeSeparators(normalized->working_directory);

    if (normalized->user_id.empty()) {
        const std::optional<std::wstring> user_sid = CurrentUserSid();
        if (!user_sid.has_value()) {
            return Failure(StartupError::QueryFailed, StartupAction::Preview,
                           HRESULT_FROM_WIN32(GetLastError()),
                           L"The current user SID could not be resolved.");
        }
        normalized->user_id = user_sid.value();
    }

    if (!IsValidXmlText(normalized->executable_path) ||
        !IsValidXmlText(normalized->arguments) ||
        !IsValidXmlText(normalized->working_directory) ||
        !IsValidXmlText(normalized->user_id)) {
        return Failure(StartupError::InvalidArgument, StartupAction::Preview,
                       E_INVALIDARG,
                       L"A startup value contains characters invalid in XML.");
    }

    return StartupStatus{};
}

std::wstring RemoveInterElementWhitespace(const std::wstring& xml) {
    std::wstring normalized;
    normalized.reserve(xml.size());

    size_t index = 0;
    while (index < xml.size()) {
        if (xml[index] != L'>') {
            normalized += xml[index++];
            continue;
        }

        normalized += xml[index++];
        const size_t whitespace_start = index;
        while (index < xml.size() && std::iswspace(xml[index])) {
            ++index;
        }
        if (index < xml.size() && xml[index] == L'<') {
            continue;
        }
        normalized.append(xml, whitespace_start, index - whitespace_start);
    }
    return normalized;
}

struct SchedulerConnection {
    ScopedComInitialization com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
};

StartupStatus ConnectScheduler(SchedulerConnection* connection,
                               StartupAction action) {
    if (!connection->com.usable()) {
        return Failure(StartupError::ComInitializationFailed, action,
                       connection->com.result(),
                       L"COM initialization failed: " +
                           HexError(connection->com.result()));
    }

    HRESULT result = CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_ITaskService,
                                      reinterpret_cast<void**>(
                                          connection->service.Put()));
    if (FAILED(result)) {
        return Failure(StartupError::TaskSchedulerUnavailable, action, result,
                       L"Task Scheduler service creation failed: " +
                           HexError(result));
    }

    VARIANT empty;
    VariantInit(&empty);
    result = connection->service->Connect(empty, empty, empty, empty);
    if (FAILED(result)) {
        return Failure(MapSchedulerError(
                           result, StartupError::TaskSchedulerUnavailable),
                       action, result,
                       L"Task Scheduler connection failed: " + HexError(result));
    }

    ScopedBstr root_path(L"\\");
    if (!root_path.valid()) {
        return Failure(StartupError::TaskSchedulerUnavailable, action,
                       E_OUTOFMEMORY,
                       L"Task Scheduler root path allocation failed.");
    }
    result = connection->service->GetFolder(root_path.Get(),
                                             connection->root.Put());
    if (FAILED(result)) {
        return Failure(MapSchedulerError(
                           result, StartupError::TaskSchedulerUnavailable),
                       action, result,
                       L"Task Scheduler root folder query failed: " +
                           HexError(result));
    }

    return StartupStatus{};
}

StartupStatus QueryTaskXml(ITaskFolder* root) {
    ScopedBstr task_name(kStartupTaskName);
    if (!task_name.valid()) {
        return Failure(StartupError::QueryFailed, StartupAction::Inspected,
                       E_OUTOFMEMORY, L"Task name allocation failed.");
    }

    ComPtr<IRegisteredTask> registered_task;
    const HRESULT result = root->GetTask(task_name.Get(), registered_task.Put());
    if (FAILED(result)) {
        if (MapSchedulerError(result, StartupError::QueryFailed) ==
            StartupError::NotFound) {
            StartupStatus status;
            status.action = StartupAction::Inspected;
            status.task_exists = false;
            return status;
        }
        return Failure(MapSchedulerError(result, StartupError::QueryFailed),
                       StartupAction::Inspected, result,
                       L"Task query failed: " + HexError(result));
    }

    BSTR xml = nullptr;
    const HRESULT xml_result = registered_task->get_Xml(&xml);
    if (FAILED(xml_result)) {
        return Failure(MapSchedulerError(xml_result, StartupError::QueryFailed),
                       StartupAction::Inspected, xml_result,
                       L"Task XML query failed: " + HexError(xml_result));
    }

    StartupStatus status;
    status.action = StartupAction::Inspected;
    status.task_exists = true;
    status.task_xml = std::wstring(xml, SysStringLen(xml));
    SysFreeString(xml);
    return status;
}

StartupStatus RegisterTaskXml(const std::wstring& xml,
                              LONG flags,
                              StartupAction success_action) {
    SchedulerConnection connection;
    StartupStatus connected = ConnectScheduler(&connection, success_action);
    if (!connected.ok()) {
        return connected;
    }

    ScopedBstr task_name(kStartupTaskName);
    ScopedBstr task_xml(xml);
    if (!task_name.valid() || !task_xml.valid()) {
        return Failure(StartupError::RegistrationFailed, success_action,
                       E_OUTOFMEMORY, L"Task registration allocation failed.");
    }

    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registered_task;
    const HRESULT result = connection.root->RegisterTask(
        task_name.Get(), task_xml.Get(), flags, empty, empty,
        TASK_LOGON_INTERACTIVE_TOKEN, empty, registered_task.Put());
    if (FAILED(result)) {
        StartupError error =
            MapSchedulerError(result, StartupError::RegistrationFailed);
        if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            error = StartupError::AlreadyExists;
        }
        return Failure(error, success_action, result,
                       L"Task registration failed: " + HexError(result));
    }

    StartupStatus status;
    status.action = success_action;
    status.task_exists = true;
    status.desired_state = true;
    status.changed = true;
    status.task_xml = xml;
    return status;
}

}  // namespace

StartupStatus BuildTaskXml(const StartupOptions& options) {
    StartupOptions normalized;
    StartupStatus validation = NormalizeOptions(options, &normalized);
    if (!validation.ok()) {
        return validation;
    }

    const std::wstring user_id = EscapeXml(normalized.user_id);
    const std::wstring executable =
        EscapeXml(normalized.executable_path);
    const std::wstring arguments = EscapeXml(normalized.arguments);
    const std::wstring working_directory =
        EscapeXml(normalized.working_directory);

    std::wstring xml;
    xml.reserve(1800 + executable.size() + arguments.size() + user_id.size());
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    xml += L"<Task version=\"1.4\" xmlns=\"";
    xml += kTaskNamespace;
    xml += L"\">\r\n";
    xml += L"  <RegistrationInfo>\r\n";
    xml += L"    <Description>Start KeySidekick at interactive user logon.</Description>\r\n";
    xml += L"  </RegistrationInfo>\r\n";
    xml += L"  <Triggers>\r\n";
    xml += L"    <LogonTrigger>\r\n";
    xml += L"      <Enabled>true</Enabled>\r\n";
    xml += L"      <UserId>" + user_id + L"</UserId>\r\n";
    xml += L"    </LogonTrigger>\r\n";
    xml += L"  </Triggers>\r\n";
    xml += L"  <Principals>\r\n";
    xml += L"    <Principal id=\"CurrentUser\">\r\n";
    xml += L"      <UserId>" + user_id + L"</UserId>\r\n";
    xml += L"      <LogonType>InteractiveToken</LogonType>\r\n";
    xml += L"      <RunLevel>LeastPrivilege</RunLevel>\r\n";
    xml += L"    </Principal>\r\n";
    xml += L"  </Principals>\r\n";
    xml += L"  <Settings>\r\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    xml += L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n";
    xml += L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n";
    xml += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
    xml += L"    <IdleSettings>\r\n";
    xml += L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n";
    xml += L"      <RestartOnIdle>false</RestartOnIdle>\r\n";
    xml += L"    </IdleSettings>\r\n";
    xml += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    xml += L"    <Enabled>true</Enabled>\r\n";
    xml += L"    <Hidden>false</Hidden>\r\n";
    xml += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
    xml += L"    <WakeToRun>false</WakeToRun>\r\n";
    xml += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n";
    xml += L"    <Priority>7</Priority>\r\n";
    xml += L"    <RestartOnFailure>\r\n";
    xml += L"      <Interval>PT1M</Interval>\r\n";
    xml += L"      <Count>3</Count>\r\n";
    xml += L"    </RestartOnFailure>\r\n";
    xml += L"  </Settings>\r\n";
    xml += L"  <Actions Context=\"CurrentUser\">\r\n";
    xml += L"    <Exec>\r\n";
    xml += L"      <Command>" + executable + L"</Command>\r\n";
    if (!arguments.empty()) {
        xml += L"      <Arguments>" + arguments + L"</Arguments>\r\n";
    }
    xml += L"      <WorkingDirectory>" + working_directory +
           L"</WorkingDirectory>\r\n";
    xml += L"    </Exec>\r\n";
    xml += L"  </Actions>\r\n";
    xml += L"</Task>\r\n";

    StartupStatus status;
    status.action = StartupAction::Preview;
    status.task_xml = std::move(xml);
    return status;
}

DesiredStateComparison CompareTaskXml(const std::wstring& existing_xml,
                                      const std::wstring& desired_xml) {
    return RemoveInterElementWhitespace(existing_xml) ==
                   RemoveInterElementWhitespace(desired_xml)
               ? DesiredStateComparison::Equivalent
               : DesiredStateComparison::Different;
}

LegacyStartupStatus DetectLegacyStartupShortcut(
    const std::wstring& startup_directory) {
    LegacyStartupStatus status;
    status.shortcut_path =
        JoinWindowsPath(startup_directory, kLegacyShortcutName);

    const DWORD attributes = GetFileAttributesW(status.shortcut_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return status;
        }
        status.error = StartupError::FilesystemError;
        status.message = L"Legacy startup shortcut could not be inspected.";
        return status;
    }
    status.exists = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return status;
}

LegacyStartupStatus DetectLegacyStartupShortcut() {
    wchar_t startup_path[MAX_PATH] = {};
    const HRESULT result = SHGetFolderPathW(
        nullptr, CSIDL_STARTUP | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT,
        startup_path);
    if (FAILED(result)) {
        LegacyStartupStatus status;
        status.error = StartupError::FilesystemError;
        status.message = L"The current user's Startup folder could not be resolved.";
        return status;
    }

    return DetectLegacyStartupShortcut(startup_path);
}

StartupManager::StartupManager(StartupManagerMode mode) noexcept : mode_(mode) {}

StartupStatus StartupManager::Inspect(const StartupOptions& desired) const {
    StartupStatus generated = BuildTaskXml(desired);
    if (!generated.ok()) {
        return generated;
    }
    if (mode_ == StartupManagerMode::TestOnly) {
        generated.action = StartupAction::Preview;
        generated.message = L"Test-only inspection generated desired task XML.";
        return generated;
    }

    SchedulerConnection connection;
    StartupStatus connected =
        ConnectScheduler(&connection, StartupAction::Inspected);
    if (!connected.ok()) {
        return connected;
    }

    StartupStatus current = QueryTaskXml(connection.root.Get());
    if (!current.ok() || !current.task_exists) {
        current.desired_state = false;
        return current;
    }

    current.desired_state =
        CompareTaskXml(current.task_xml.value(), generated.task_xml.value()) ==
        DesiredStateComparison::Equivalent;
    return current;
}

StartupStatus StartupManager::Create(const StartupOptions& desired) const {
    StartupStatus generated = BuildTaskXml(desired);
    if (!generated.ok()) {
        return generated;
    }
    if (mode_ == StartupManagerMode::TestOnly) {
        generated.action = StartupAction::Preview;
        generated.message = L"Test-only create generated task XML without registration.";
        return generated;
    }
    return RegisterTaskXml(generated.task_xml.value(), TASK_CREATE,
                           StartupAction::Created);
}

StartupStatus StartupManager::Update(const StartupOptions& desired) const {
    StartupStatus generated = BuildTaskXml(desired);
    if (!generated.ok()) {
        return generated;
    }
    if (mode_ == StartupManagerMode::TestOnly) {
        generated.action = StartupAction::Preview;
        generated.message = L"Test-only update generated task XML without registration.";
        return generated;
    }
    return RegisterTaskXml(generated.task_xml.value(), TASK_UPDATE,
                           StartupAction::Updated);
}

StartupStatus StartupManager::Remove() const {
    if (mode_ == StartupManagerMode::TestOnly) {
        StartupStatus status;
        status.action = StartupAction::Preview;
        status.message = L"Test-only remove did not access Task Scheduler.";
        return status;
    }

    SchedulerConnection connection;
    StartupStatus connected = ConnectScheduler(&connection, StartupAction::Removed);
    if (!connected.ok()) {
        return connected;
    }

    ScopedBstr task_name(kStartupTaskName);
    if (!task_name.valid()) {
        return Failure(StartupError::RemovalFailed, StartupAction::Removed,
                       E_OUTOFMEMORY, L"Task name allocation failed.");
    }

    const HRESULT result = connection.root->DeleteTask(task_name.Get(), 0);
    if (FAILED(result)) {
        return Failure(MapSchedulerError(result, StartupError::RemovalFailed),
                       StartupAction::Removed, result,
                       L"Task removal failed: " + HexError(result));
    }

    StartupStatus status;
    status.action = StartupAction::Removed;
    status.changed = true;
    return status;
}

StartupStatus StartupManager::Ensure(const StartupOptions& desired) const {
    StartupStatus generated = BuildTaskXml(desired);
    if (!generated.ok()) {
        return generated;
    }
    if (mode_ == StartupManagerMode::TestOnly) {
        generated.action = StartupAction::Preview;
        generated.message = L"Test-only ensure generated task XML without registration.";
        return generated;
    }

    StartupStatus inspected = Inspect(desired);
    if (!inspected.ok()) {
        return inspected;
    }
    if (!inspected.task_exists) {
        return Create(desired);
    }
    if (!inspected.desired_state) {
        return Update(desired);
    }

    inspected.action = StartupAction::Unchanged;
    inspected.changed = false;
    inspected.message = L"The startup task already matches the desired state.";
    return inspected;
}

}  // namespace keysidekick
