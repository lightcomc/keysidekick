#include "../src/runtime_storage.h"

#include <windows.h>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error(std::string("CHECK failed: ") + #condition); \
        }                                                                       \
    } while (0)

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (left[left.size() - 1] == L'\\' || left[left.size() - 1] == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void RemoveTree(const std::wstring& directory) {
    WIN32_FIND_DATAW entry = {};
    const std::wstring pattern = JoinPath(directory, L"*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 ||
                wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring path = JoinPath(directory, entry.cFileName);
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &entry) != FALSE);
        FindClose(find);
    }
    RemoveDirectoryW(directory.c_str());
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        wchar_t temp_path[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp_path);
        CHECK(length > 0 && length < MAX_PATH);

        wchar_t unique_path[MAX_PATH + 1] = {};
        CHECK(GetTempFileNameW(temp_path, L"ksk", 0, unique_path) != 0);
        CHECK(DeleteFileW(unique_path) != FALSE);
        CHECK(CreateDirectoryW(unique_path, NULL) != FALSE);
        path_ = unique_path;
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) RemoveTree(path_);
    }

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

std::string Read(const std::wstring& path) {
    std::string content;
    const keysidekick::StorageResult result =
        keysidekick::ReadUtf8File(path, &content);
    CHECK(result.ok());
    return content;
}

void WriteRaw(const std::wstring& path, const std::string& content) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);

    size_t offset = 0;
    while (offset < content.size()) {
        const size_t remaining = content.size() - offset;
        const DWORD chunk = remaining > 0x7ffffffful
                                ? 0x7ffffffful
                                : static_cast<DWORD>(remaining);
        DWORD written = 0;
        CHECK(WriteFile(file, content.data() + offset, chunk, &written, NULL) != FALSE);
        CHECK(written != 0);
        offset += written;
    }
    CHECK(CloseHandle(file) != FALSE);
}

size_t CountTemporaryFiles(const std::wstring& directory) {
    WIN32_FIND_DATAW entry = {};
    HANDLE find = FindFirstFileW(JoinPath(directory, L"*.tmp.*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return 0;

    size_t count = 1;
    while (FindNextFileW(find, &entry) != FALSE) ++count;
    FindClose(find);
    return count;
}

keysidekick::Utf8Validator AcceptAll() {
    return [](const std::string&, std::string*) { return true; };
}

void TestRuntimePathsAndDirectoriesUseExplicitRoots() {
    TemporaryDirectory temporary;
    const std::wstring executable_dir = JoinPath(temporary.path(), L"bin");
    const std::wstring local_app_data = JoinPath(temporary.path(), L"local");
    CHECK(CreateDirectoryW(executable_dir.c_str(), NULL) != FALSE);
    CHECK(CreateDirectoryW(local_app_data.c_str(), NULL) != FALSE);

    keysidekick::RuntimePaths paths;
    keysidekick::StorageResult result = keysidekick::BuildRuntimePaths(
        executable_dir, local_app_data, &paths);
    CHECK(result.ok());
    CHECK(paths.executableDir == executable_dir);
    CHECK(paths.dataDir == JoinPath(local_app_data, L"KeySidekick"));
    CHECK(paths.logDir == JoinPath(paths.dataDir, L"logs"));

    result = keysidekick::EnsureRuntimeDirectories(paths);
    CHECK(result.ok());
    CHECK(IsDirectory(paths.dataDir));
    CHECK(IsDirectory(paths.logDir));

    result = keysidekick::BuildRuntimePaths(executable_dir, L"", &paths);
    CHECK(!result.ok());
    CHECK(result.stage == keysidekick::StorageStage::ResolveLocalAppData);
    CHECK(result.win32Error != ERROR_SUCCESS);
    CHECK(!result.message.empty());
}

void TestInitialWrite() {
    TemporaryDirectory temporary;
    const std::wstring target = JoinPath(temporary.path(), L"config.ini");

    const keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(
        target, "first", AcceptAll());

    CHECK(result.ok());
    CHECK(Read(target) == "first");
    CHECK(!PathExists(target + L".bak"));
    CHECK(CountTemporaryFiles(temporary.path()) == 0);
}

void TestReplacementCreatesBackup() {
    TemporaryDirectory temporary;
    const std::wstring target = JoinPath(temporary.path(), L"config.ini");
    CHECK(keysidekick::AtomicWriteUtf8(target, "old", AcceptAll()).ok());

    const keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(
        target, "new", AcceptAll());

    CHECK(result.ok());
    CHECK(Read(target) == "new");
    CHECK(Read(target + L".bak") == "old");
    CHECK(CountTemporaryFiles(temporary.path()) == 0);
}

void TestValidatorRejectionPreservesOriginal() {
    TemporaryDirectory temporary;
    const std::wstring target = JoinPath(temporary.path(), L"config.ini");
    CHECK(keysidekick::AtomicWriteUtf8(target, "original", AcceptAll()).ok());

    const keysidekick::Utf8Validator reject =
        [](const std::string&, std::string* reason) {
            if (reason != NULL) *reason = "invalid config";
            return false;
        };
    const keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(
        target, "rejected", reject);

    CHECK(!result.ok());
    CHECK(result.stage == keysidekick::StorageStage::ValidateTemp);
    CHECK(result.message.find("invalid config") != std::string::npos);
    CHECK(Read(target) == "original");
    CHECK(!PathExists(target + L".bak"));
    CHECK(CountTemporaryFiles(temporary.path()) == 0);
}

void TestMigrationDoesNotOverwriteDestination() {
    TemporaryDirectory temporary;
    const std::wstring source = JoinPath(temporary.path(), L"legacy.ini");
    const std::wstring target = JoinPath(temporary.path(), L"config.ini");
    WriteRaw(source, "legacy");

    keysidekick::MigrationResult migration =
        keysidekick::MigrateLegacyConfigIfNeeded(source, target, AcceptAll());
    CHECK(migration.status == keysidekick::MigrationStatus::Migrated);
    CHECK(migration.result.ok());
    CHECK(Read(target) == "legacy");
    CHECK(Read(source) == "legacy");

    WriteRaw(source, "changed legacy");
    migration = keysidekick::MigrateLegacyConfigIfNeeded(
        source, target, AcceptAll());
    CHECK(migration.status == keysidekick::MigrationStatus::Skipped);
    CHECK(migration.result.ok());
    CHECK(Read(target) == "legacy");
    CHECK(CountTemporaryFiles(temporary.path()) == 0);
}

void TestUnicodeDirectoryRoundTrip() {
    TemporaryDirectory temporary;
    const std::wstring unicode_dir =
        JoinPath(temporary.path(), L"\u0434\u0430\u043d\u043d\u044b\u0435-\u952e\u76d8");
    CHECK(CreateDirectoryW(unicode_dir.c_str(), NULL) != FALSE);
    const std::wstring target = JoinPath(unicode_dir, L"\u043a\u043e\u043d\u0444\u0438\u0433.ini");
    const std::string content = u8"\u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435-\u952e\u76d8";

    const keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(
        target, content, AcceptAll());

    CHECK(result.ok());
    CHECK(Read(target) == content);
    CHECK(CountTemporaryFiles(unicode_dir) == 0);
}

void TestRejectedMigrationCleansTemporaryFile() {
    TemporaryDirectory temporary;
    const std::wstring source = JoinPath(temporary.path(), L"legacy.ini");
    const std::wstring target = JoinPath(temporary.path(), L"config.ini");
    WriteRaw(source, "bad legacy");

    const keysidekick::Utf8Validator reject =
        [](const std::string&, std::string* reason) {
            if (reason != NULL) *reason = "legacy rejected";
            return false;
        };
    const keysidekick::MigrationResult migration =
        keysidekick::MigrateLegacyConfigIfNeeded(source, target, reject);

    CHECK(migration.status == keysidekick::MigrationStatus::Error);
    CHECK(!migration.result.ok());
    CHECK(!PathExists(target));
    CHECK(Read(source) == "bad legacy");
    CHECK(CountTemporaryFiles(temporary.path()) == 0);
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

}  // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"runtime paths and directories", TestRuntimePathsAndDirectoriesUseExplicitRoots},
        {"initial write", TestInitialWrite},
        {"replacement creates backup", TestReplacementCreatesBackup},
        {"validator rejection preserves original", TestValidatorRejectionPreservesOriginal},
        {"migration no-overwrite", TestMigrationDoesNotOverwriteDestination},
        {"Unicode directory", TestUnicodeDirectoryRoundTrip},
        {"temporary cleanup", TestRejectedMigrationCleansTemporaryFile},
    };

    int failures = 0;
    for (size_t index = 0; index < tests.size(); ++index) {
        try {
            tests[index].run();
            std::cout << "PASS: " << tests[index].name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << tests[index].name << ": "
                      << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
