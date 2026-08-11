#include "../src/app_instance.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using keysidekick::AppInstance;
using keysidekick::AppInstanceAcquireResult;
using keysidekick::AppInstanceAcquireStatus;
using keysidekick::AppInstanceNames;
using keysidekick::AppLaunchMode;
using keysidekick::BuildAppInstanceNames;
using keysidekick::ParseAppLaunchMode;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;

    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

std::wstring UniqueTestAppName(const wchar_t* testName) {
    return std::wstring(L"KeySidekick.AppInstanceTests.") +
           std::to_wstring(GetCurrentProcessId()) + L"." + testName;
}

void TestStableCollisionResistantNames() {
    const char* testName = "stable collision-resistant names";
    const AppInstanceNames first =
        BuildAppInstanceNames(L"KeySidekick Desktop", L"release/channel");
    const AppInstanceNames same =
        BuildAppInstanceNames(L"KeySidekick Desktop", L"release/channel");
    const AppInstanceNames other =
        BuildAppInstanceNames(L"KeySidekick Desktop", L"release-channel");

    REQUIRE(!first.mutex_name.empty());
    REQUIRE(first.mutex_name == same.mutex_name);
    REQUIRE(first.open_event_name == same.open_event_name);
    REQUIRE(first.window_message_name == same.window_message_name);
    REQUIRE(first.mutex_name != other.mutex_name);
    REQUIRE(first.open_event_name != other.open_event_name);
    REQUIRE(first.window_message_name != other.window_message_name);
    REQUIRE(first.mutex_name.find(L"Global\\") != 0);
    REQUIRE(first.open_event_name.find(L"Global\\") != 0);
    REQUIRE(first.mutex_name.find(L"Local\\") == 0);
    REQUIRE(first.open_event_name.find(L"Local\\") == 0);
}

void TestArgumentModeParsing() {
    const char* testName = "argument mode parsing";

    REQUIRE(ParseAppLaunchMode(std::vector<std::wstring>()) ==
            AppLaunchMode::Default);
    REQUIRE(ParseAppLaunchMode(std::vector<std::wstring>(1, L"--agent")) ==
            AppLaunchMode::Agent);
    REQUIRE(ParseAppLaunchMode(std::vector<std::wstring>(1, L"--supervise")) ==
            AppLaunchMode::Supervise);
    REQUIRE(ParseAppLaunchMode(
                std::vector<std::wstring>(1, L"--open-dashboard")) ==
            AppLaunchMode::OpenDashboard);
    REQUIRE(ParseAppLaunchMode(
                std::vector<std::wstring>(1, L"--diagnostics")) ==
            AppLaunchMode::Diagnostics);

    std::vector<std::wstring> arguments;
    arguments.push_back(L"--unknown");
    arguments.push_back(L"--AGENT");
    REQUIRE(ParseAppLaunchMode(arguments) == AppLaunchMode::Agent);

    arguments.clear();
    arguments.push_back(L"--agent");
    arguments.push_back(L"--diagnostics");
    REQUIRE(ParseAppLaunchMode(arguments) == AppLaunchMode::Diagnostics);
}

void TestAcquireSignalCleanupAndReacquire() {
    const char* testName = "acquire signal cleanup and reacquire";
    const std::wstring appName = UniqueTestAppName(L"lifecycle");
    const std::wstring suffix = L"pid-" + std::to_wstring(GetCurrentProcessId());

    AppInstance second(appName, suffix);
    {
        AppInstance first(appName, suffix);
        const AppInstanceAcquireResult firstResult = first.Acquire();
        REQUIRE(firstResult.status == AppInstanceAcquireStatus::Acquired);
        REQUIRE(firstResult.win32_error == ERROR_SUCCESS);
        REQUIRE(first.owns_instance());
        REQUIRE(first.window_message() != 0);
        REQUIRE(!first.ConsumeOpenDashboardRequest());

        const AppInstanceAcquireResult secondResult = second.Acquire();
        REQUIRE(secondResult.status ==
                AppInstanceAcquireStatus::AlreadyRunning);
        REQUIRE(secondResult.win32_error == ERROR_SUCCESS);
        REQUIRE(!second.owns_instance());
        REQUIRE(second.RequestOpenDashboard());
        REQUIRE(first.ConsumeOpenDashboardRequest());
        REQUIRE(!first.ConsumeOpenDashboardRequest());
    }

    AppInstance replacement(appName, suffix);
    const AppInstanceAcquireResult replacementResult = replacement.Acquire();
    REQUIRE(replacementResult.status == AppInstanceAcquireStatus::Acquired);
    REQUIRE(replacement.owns_instance());
}

void TestAcquireIsIdempotentForOwner() {
    const char* testName = "acquire is idempotent for owner";
    AppInstance instance(UniqueTestAppName(L"idempotent"),
                         std::to_wstring(GetCurrentProcessId()));

    REQUIRE(instance.Acquire().status == AppInstanceAcquireStatus::Acquired);
    REQUIRE(instance.Acquire().status == AppInstanceAcquireStatus::Acquired);
    REQUIRE(instance.owns_instance());
}

void TestInvalidNameReturnsError() {
    const char* testName = "invalid name returns error";
    AppInstance instance(L"", std::to_wstring(GetCurrentProcessId()));
    const AppInstanceAcquireResult result = instance.Acquire();

    REQUIRE(result.status == AppInstanceAcquireStatus::Error);
    REQUIRE(result.win32_error == ERROR_INVALID_PARAMETER);
    REQUIRE(!instance.owns_instance());
    REQUIRE(!instance.RequestOpenDashboard());
    REQUIRE(!instance.ConsumeOpenDashboardRequest());
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"stable collision-resistant names", TestStableCollisionResistantNames},
        {"argument mode parsing", TestArgumentModeParsing},
        {"acquire signal cleanup and reacquire", TestAcquireSignalCleanupAndReacquire},
        {"acquire is idempotent for owner", TestAcquireIsIdempotentForOwner},
        {"invalid name returns error", TestInvalidNameReturnsError},
    };

    for (const TestCase& test : tests) {
        test.run();
        std::cout << "PASS: " << test.name << '\n';
    }

    std::cout << "All app instance tests passed (5/5).\n";
    return 0;
}
