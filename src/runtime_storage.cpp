#include "runtime_storage.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace keysidekick {
namespace {

std::wstring JoinPath(const std::wstring& left, const wchar_t* right) {
    if (left.empty()) return right;
    std::wstring result = left;
    if (result[result.size()-1] != L'\\' && result[result.size()-1] != L'/')
        result += L'\\';
    result += right;
    return result;
}

std::wstring UniqueTempPath(const std::wstring& target) {
    const size_t dot = target.find_last_of(L'.');
    const size_t sep = target.find_last_of(L"\\/");
    std::wstring base = target;
    std::wstring ext;
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep)) {
        base = target.substr(0, dot);
        ext = target.substr(dot);
    }
    wchar_t suffix[40];
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    swprintf(suffix, sizeof(suffix)/sizeof(suffix[0]),
             L".tmp_%04u%02u%02u%02u%02u%02u%03u_%lu",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, pid);
    return base + suffix + ext;
}

bool ReadFileContents(const std::wstring& path, std::string* content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    content->resize(static_cast<size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < content->size()) {
        DWORD to_read = static_cast<DWORD>(content->size() - total);
        if (to_read > 0x400000) to_read = 0x400000;
        DWORD got = 0;
        if (!ReadFile(h, &(*content)[total], to_read, &got, NULL) || got == 0) break;
        total += got;
    }
    content->resize(total);
    CloseHandle(h);
    return true;
}

} // namespace

StorageResult BuildRuntimePaths(const std::wstring& executable_dir,
                                const std::wstring& local_app_data,
                                RuntimePaths* paths) {
    StorageResult result;
    if (executable_dir.empty()) {
        result.stage = ResolveLocalAppData;
        result.win32Error = ERROR_INVALID_PARAMETER;
        result.message = "executable directory is empty";
        return result;
    }
    if (local_app_data.empty()) {
        result.stage = ResolveLocalAppData;
        result.win32Error = ERROR_INVALID_PARAMETER;
        result.message = "LOCALAPPDATA is empty";
        return result;
    }

    paths->executableDir = executable_dir;
    paths->dataDir = JoinPath(local_app_data, L"KeySidekick");
    paths->logDir = JoinPath(paths->dataDir, L"logs");
    paths->configPath = JoinPath(paths->dataDir, L"config.ini");
    paths->logPath = JoinPath(paths->logDir, L"sidekick.log");
    result.stage = StageNone;
    result.win32Error = ERROR_SUCCESS;
    return result;
}

StorageResult EnsureRuntimeDirectories(const RuntimePaths& paths) {
    StorageResult result;
    result.stage = CreateDirectories;
    result.win32Error = ERROR_SUCCESS;

    if (!paths.dataDir.empty()) {
        if (!CreateDirectoryW(paths.dataDir.c_str(), NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                result.win32Error = err;
                result.message = "Failed to create data directory";
                return result;
            }
        }
    }
    if (!paths.logDir.empty() && paths.logDir != paths.dataDir) {
        if (!CreateDirectoryW(paths.logDir.c_str(), NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                result.win32Error = err;
                result.message = "Failed to create log directory";
                return result;
            }
        }
    }
    result.stage = StageNone;
    return result;
}

StorageResult ReadUtf8File(const std::wstring& path, std::string* content) {
    StorageResult result;
    if (!ReadFileContents(path, content)) {
        result.win32Error = GetLastError();
        result.message = "Failed to read file";
        result.stage = ReadSource;
        content->clear();
    }
    return result;
}

StorageResult AtomicWriteUtf8(const std::wstring& target_path,
                              const std::string& content,
                              const Utf8Validator& validator) {
    StorageResult result;
    result.win32Error = ERROR_SUCCESS;

    const std::wstring temp_path = UniqueTempPath(target_path);
    result.tempPath = temp_path;

    // Open temp file for writing
    result.stage = OpenTempForWrite;
    HANDLE h = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        result.win32Error = GetLastError();
        result.message = "Failed to create temp file";
        return result;
    }

    // Write content
    result.stage = WriteTemp;
    DWORD written_total = 0;
    while (written_total < content.size()) {
        DWORD to_write = static_cast<DWORD>(content.size() - written_total);
        if (to_write > 0x400000) to_write = 0x400000;
        DWORD wrote = 0;
        if (!WriteFile(h, content.data() + written_total, to_write, &wrote, NULL) || wrote == 0) {
            result.win32Error = GetLastError();
            result.message = "WriteFile failed";
            CloseHandle(h);
            DeleteFileW(temp_path.c_str());
            return result;
        }
        written_total += wrote;
    }

    // Flush
    result.stage = FlushTemp;
    if (!FlushFileBuffers(h)) {
        result.win32Error = GetLastError();
        result.message = "FlushFileBuffers failed";
        CloseHandle(h);
        DeleteFileW(temp_path.c_str());
        return result;
    }
    CloseHandle(h);

    // Validate temp content
    result.stage = ValidateTemp;
    std::string verify;
    if (!ReadFileContents(temp_path, &verify) || verify != content) {
        result.win32Error = ERROR_CRC;
        result.message = "Temp content verification failed";
        DeleteFileW(temp_path.c_str());
        return result;
    }

    // Run user validator
    if (validator) {
        std::string reason;
        if (!validator(verify, &reason)) {
            result.win32Error = ERROR_INVALID_DATA;
            result.message = reason.empty() ? "Validator rejected content" : reason;
            DeleteFileW(temp_path.c_str());
            return result;
        }
    }

    // Atomic replace
    result.stage = ReplaceTarget;
    result.tempPath.clear(); // about to be moved/replaced
    bool target_exists = (GetFileAttributesW(target_path.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (target_exists) {
        std::wstring backup_path = target_path + L".bak";
        // Delete stale backup so ReplaceFileW can create a fresh one
        DeleteFileW(backup_path.c_str());
        if (ReplaceFileW(target_path.c_str(), temp_path.c_str(),
                         backup_path.c_str(), 0, NULL, NULL)) {
            result.stage = StageNone;
            return result;
        }
        // Fallback: MoveFileExW with replace
        if (MoveFileExW(temp_path.c_str(), target_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            result.stage = StageNone;
            return result;
        }
        result.win32Error = GetLastError();
        result.message = "ReplaceFileW and MoveFileExW both failed";
        DeleteFileW(temp_path.c_str());
        return result;
    } else {
        if (MoveFileExW(temp_path.c_str(), target_path.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
            result.stage = StageNone;
            return result;
        }
        result.win32Error = GetLastError();
        result.message = "MoveFileExW failed for new file";
        DeleteFileW(temp_path.c_str());
        return result;
    }
}

MigrationResult MigrateLegacyConfigIfNeeded(const std::wstring& source_path,
                                            const std::wstring& target_path,
                                            const Utf8Validator& validator) {
    MigrationResult migration;

    // If target already exists, skip
    if (GetFileAttributesW(target_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        migration.status = Skipped;
        migration.result.win32Error = ERROR_SUCCESS;
        migration.result.message = "Destination already exists";
        return migration;
    }

    // Read source
    std::string content;
    if (!ReadFileContents(source_path, &content)) {
        migration.status = Error;
        migration.result.win32Error = GetLastError();
        migration.result.message = "Failed to read legacy config";
        migration.result.stage = ReadSource;
        return migration;
    }

    // Write to target atomically
    migration.result = AtomicWriteUtf8(target_path, content, validator);
    if (!migration.result.ok()) {
        migration.status = Error;
        return migration;
    }

    migration.status = Migrated;
    return migration;
}

} // namespace keysidekick
