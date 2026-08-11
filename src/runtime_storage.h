#ifndef KEYSIDEKICK_RUNTIME_STORAGE_H
#define KEYSIDEKICK_RUNTIME_STORAGE_H

#include <functional>
#include <string>
#include <windows.h>

namespace keysidekick {

typedef std::function<bool(const std::string&, std::string*)> Utf8Validator;

enum StorageStage {
    StageNone = 0,
    ResolveLocalAppData,
    CreateDirectories,
    OpenTempForWrite,
    WriteTemp,
    FlushTemp,
    ValidateTemp,
    ReplaceTarget,
    ReadSource,
    Cleanup
};

enum MigrationStatus {
    MigrationNone = 0,
    Migrated,
    Skipped,
    Error
};

struct StorageResult {
    StorageStage stage;
    DWORD win32Error;
    std::string message;
    std::wstring tempPath;

    StorageResult() : stage(StageNone), win32Error(0) {}
    bool ok() const { return win32Error == 0 && stage != StageNone
                          ? false : win32Error == 0; }
};

struct RuntimePaths {
    std::wstring executableDir;
    std::wstring dataDir;
    std::wstring logDir;
    std::wstring configPath;
    std::wstring logPath;
};

StorageResult BuildRuntimePaths(const std::wstring& executable_dir,
                                const std::wstring& local_app_data,
                                RuntimePaths* paths);

StorageResult EnsureRuntimeDirectories(const RuntimePaths& paths);

StorageResult ReadUtf8File(const std::wstring& path, std::string* content);

StorageResult AtomicWriteUtf8(const std::wstring& target_path,
                              const std::string& content,
                              const Utf8Validator& validator);

struct MigrationResult {
    MigrationStatus status;
    StorageResult result;
    MigrationResult() : status(MigrationNone) {}
};

MigrationResult MigrateLegacyConfigIfNeeded(const std::wstring& source_path,
                                            const std::wstring& target_path,
                                            const Utf8Validator& validator);

} // namespace keysidekick

#endif
