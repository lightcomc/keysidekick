#include "../src/startup_manager.h"

#include <windows.h>

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectContains(const std::wstring& value,
                    const std::wstring& expected,
                    const char* message) {
    Expect(value.find(expected) != std::wstring::npos, message);
}

void TestDesiredFlagsAndXmlEscaping() {
    keysidekick::StartupOptions options;
    options.executable_path = LR"(C:\Apps & Tools\KeySidekick\sidekick.exe)";
    options.arguments = LR"(--label <primary> --config "C:\A&B\config.ini")";
    options.working_directory = LR"(C:\Apps & Tools\KeySidekick)";
    options.user_id = LR"(DOMAIN\User & Admin)";

    const keysidekick::StartupStatus status =
        keysidekick::BuildTaskXml(options);

    Expect(status.ok(), "XML generation succeeds");
    Expect(status.action == keysidekick::StartupAction::Preview,
           "XML generation reports preview action");
    Expect(status.changed == false, "XML generation does not mutate state");
    Expect(status.task_xml.has_value(), "XML generation returns XML");

    const std::wstring& xml = status.task_xml.value();
    ExpectContains(xml, L"<LogonTrigger>", "uses a logon trigger");
    ExpectContains(xml, L"<UserId>DOMAIN\\User &amp; Admin</UserId>",
                   "escapes the current user id");
    ExpectContains(xml, L"<LogonType>InteractiveToken</LogonType>",
                   "requires an interactive token");
    ExpectContains(xml, L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>",
                   "ignores duplicate launches");
    ExpectContains(xml, L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>",
                   "allows unlimited execution");
    ExpectContains(xml, L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>",
                   "allows starting on battery");
    ExpectContains(xml, L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>",
                   "allows running on battery");
    ExpectContains(xml, L"<RestartOnFailure>", "configures restart on failure");
    ExpectContains(xml, L"<Interval>PT1M</Interval>",
                   "uses a deterministic restart interval");
    ExpectContains(xml, L"<Count>3</Count>",
                   "uses a deterministic restart count");
    ExpectContains(xml,
                   L"<Command>C:\\Apps &amp; Tools\\KeySidekick\\sidekick.exe</Command>",
                   "escapes the executable path");
    ExpectContains(xml,
                   L"<Arguments>--label &lt;primary&gt; --config &quot;C:\\A&amp;B\\config.ini&quot;</Arguments>",
                   "escapes arguments without shell quoting");
}

void TestRejectsRelativeExecutablePath() {
    keysidekick::StartupOptions options;
    options.executable_path = L"sidekick.exe";
    options.user_id = L"User";

    const keysidekick::StartupStatus status =
        keysidekick::BuildTaskXml(options);

    Expect(!status.ok(), "relative executable path is rejected");
    Expect(status.error == keysidekick::StartupError::InvalidArgument,
           "relative path returns invalid argument");
    Expect(!status.task_xml.has_value(), "invalid request returns no XML");
}

void TestLegacyShortcutDetectionWithTempPath() {
    wchar_t temp_path[MAX_PATH] = {};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp_path);
    Expect(temp_length > 0 && temp_length < MAX_PATH,
           "temporary directory can be resolved");

    const std::wstring root =
        std::wstring(temp_path) + L"keysidekick-startup-tests-" +
        std::to_wstring(GetCurrentProcessId());
    const std::wstring startup = root + L"\\Startup";
    const std::wstring shortcut = startup + L"\\KeySidekick.lnk";
    DeleteFileW(shortcut.c_str());
    RemoveDirectoryW(startup.c_str());
    RemoveDirectoryW(root.c_str());
    Expect(CreateDirectoryW(root.c_str(), nullptr) != FALSE,
           "temporary test root is created");
    Expect(CreateDirectoryW(startup.c_str(), nullptr) != FALSE,
           "temporary Startup directory is created");

    keysidekick::LegacyStartupStatus missing =
        keysidekick::DetectLegacyStartupShortcut(startup);
    Expect(missing.error == keysidekick::StartupError::None,
           "missing shortcut is not an error");
    Expect(!missing.exists, "missing shortcut is reported absent");

    HANDLE shortcut_file = CreateFileW(
        shortcut.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(shortcut_file != INVALID_HANDLE_VALUE,
           "temporary shortcut placeholder is created");
    if (shortcut_file != INVALID_HANDLE_VALUE) {
        CloseHandle(shortcut_file);
    }

    keysidekick::LegacyStartupStatus found =
        keysidekick::DetectLegacyStartupShortcut(startup);
    Expect(found.error == keysidekick::StartupError::None,
           "existing shortcut detection succeeds");
    Expect(found.exists, "existing legacy shortcut is detected");
    Expect(found.shortcut_path == shortcut,
           "legacy helper returns the exact shortcut path");

    DeleteFileW(shortcut.c_str());
    RemoveDirectoryW(startup.c_str());
    RemoveDirectoryW(root.c_str());
}

void TestDesiredStateComparisonIsIdempotent() {
    keysidekick::StartupOptions options;
    options.executable_path = LR"(C:\Program Files\KeySidekick\sidekick.exe)";
    options.arguments = L"--quiet";
    options.working_directory = LR"(C:\Program Files\KeySidekick)";
    options.user_id = LR"(DOMAIN\User)";

    const keysidekick::StartupStatus generated =
        keysidekick::BuildTaskXml(options);
    Expect(generated.ok(), "desired XML generation succeeds");

    const keysidekick::DesiredStateComparison same =
        keysidekick::CompareTaskXml(generated.task_xml.value(),
                                    generated.task_xml.value());
    Expect(same == keysidekick::DesiredStateComparison::Equivalent,
           "identical desired state is equivalent");

    std::wstring formatted = generated.task_xml.value();
    const size_t insertion = formatted.find(L"><");
    if (insertion != std::wstring::npos) {
        formatted.replace(insertion, 2, L">\r\n  <");
    }
    const keysidekick::DesiredStateComparison whitespace_only =
        keysidekick::CompareTaskXml(formatted, generated.task_xml.value());
    Expect(whitespace_only == keysidekick::DesiredStateComparison::Equivalent,
           "insignificant inter-element whitespace is ignored");

    std::wstring changed = generated.task_xml.value();
    const size_t policy = changed.find(L"IgnoreNew");
    changed.replace(policy, std::wstring(L"IgnoreNew").size(), L"Parallel");
    const keysidekick::DesiredStateComparison different =
        keysidekick::CompareTaskXml(changed, generated.task_xml.value());
    Expect(different == keysidekick::DesiredStateComparison::Different,
           "semantic desired-state changes require update");
}

void TestExplicitPreviewOperationsDoNotMutateSystem() {
    keysidekick::StartupOptions options;
    options.executable_path = LR"(C:\KeySidekick\sidekick.exe)";
    options.user_id = L"User";

    keysidekick::StartupManager manager(
        keysidekick::StartupManagerMode::TestOnly);

    const keysidekick::StartupStatus ensure = manager.Ensure(options);
    Expect(ensure.ok(), "test-only ensure succeeds");
    Expect(ensure.action == keysidekick::StartupAction::Preview,
           "test-only ensure is a preview");
    Expect(!ensure.changed, "test-only ensure never changes the system");
    Expect(ensure.task_xml.has_value(), "test-only ensure returns XML");

    const keysidekick::StartupStatus remove = manager.Remove();
    Expect(remove.ok(), "test-only remove succeeds");
    Expect(remove.action == keysidekick::StartupAction::Preview,
           "test-only remove is a preview");
    Expect(!remove.changed, "test-only remove never changes the system");
}

}  // namespace

int main() {
    TestDesiredFlagsAndXmlEscaping();
    TestRejectsRelativeExecutablePath();
    TestLegacyShortcutDetectionWithTempPath();
    TestDesiredStateComparisonIsIdempotent();
    TestExplicitPreviewOperationsDoNotMutateSystem();

    if (failures != 0) {
        std::cerr << failures << " startup manager test(s) failed\n";
        return 1;
    }

    std::cout << "All startup manager tests passed\n";
    return 0;
}
