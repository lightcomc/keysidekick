#include "../src/windows_targets.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using keysidekick::windows_targets::BuildWindowsCommandLine;
using keysidekick::windows_targets::EnumerateWindows;
using keysidekick::windows_targets::IsWindowCandidateAllowed;
using keysidekick::windows_targets::LaunchApplication;
using keysidekick::windows_targets::LaunchedProcess;
using keysidekick::windows_targets::ResolveTarget;
using keysidekick::windows_targets::ScoreTargetCandidate;
using keysidekick::windows_targets::TargetQuery;
using keysidekick::windows_targets::WindowCandidate;
using keysidekick::windows_targets::WindowFilterPolicy;

void Require(bool condition, const char* expression, const char* testName) {
    if (condition) return;

    std::cerr << "FAILED: " << testName << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(condition) Require((condition), #condition, testName)

WindowCandidate Candidate(std::uintptr_t handle,
                          const wchar_t* windowClass,
                          const wchar_t* processName,
                          const wchar_t* processPath) {
    WindowCandidate candidate;
    candidate.handle = handle;
    candidate.title = L"Synthetic window";
    candidate.windowClass = windowClass;
    candidate.processId = static_cast<std::uint32_t>(handle + 100);
    candidate.processName = processName;
    candidate.processPath = processPath;
    candidate.visible = true;
    return candidate;
}

void TestDefaultFilteringPolicy() {
    const char* testName = "default filtering policy";
    const WindowFilterPolicy policy;
    WindowCandidate candidate = Candidate(1, L"EditorWindow", L"editor.exe", L"C:\\Apps\\editor.exe");

    REQUIRE(IsWindowCandidateAllowed(candidate, policy));

    candidate.visible = false;
    REQUIRE(!IsWindowCandidateAllowed(candidate, policy));
    candidate.visible = true;

    candidate.title.clear();
    REQUIRE(!IsWindowCandidateAllowed(candidate, policy));
    candidate.title = L"Synthetic window";

    candidate.toolWindow = true;
    REQUIRE(!IsWindowCandidateAllowed(candidate, policy));
    candidate.toolWindow = false;

    candidate.shellWindow = true;
    REQUIRE(!IsWindowCandidateAllowed(candidate, policy));
}

void TestFilteringPolicyIsConfigurable() {
    const char* testName = "filtering policy is configurable";
    WindowCandidate candidate;
    candidate.toolWindow = true;
    candidate.shellWindow = true;

    WindowFilterPolicy policy;
    policy.requireVisible = false;
    policy.excludeEmptyTitles = false;
    policy.excludeToolWindows = false;
    policy.excludeShellWindows = false;

    REQUIRE(IsWindowCandidateAllowed(candidate, policy));
}

void TestExactClassIsRequired() {
    const char* testName = "exact class is required";
    const WindowCandidate candidate = Candidate(2, L"PlayerMainWindow", L"player.exe", L"C:\\Player\\player.exe");

    TargetQuery query;
    query.windowClass = L"PlayerMain";
    REQUIRE(ScoreTargetCandidate(candidate, query) < 0);

    query.windowClass = L"PlayerMainWindow";
    REQUIRE(ScoreTargetCandidate(candidate, query) >= 0);
}

void TestOptionalProcessNameIsExact() {
    const char* testName = "optional process name is exact";
    const WindowCandidate candidate = Candidate(3, L"SharedWindow", L"music.exe", L"C:\\Music\\music.exe");

    TargetQuery query;
    query.windowClass = L"SharedWindow";
    query.processName = L"video.exe";
    REQUIRE(ScoreTargetCandidate(candidate, query) < 0);

    query.processName = L"music.exe";
    REQUIRE(ScoreTargetCandidate(candidate, query) >= 0);
}

void TestProcessNameCanComeFromPath() {
    const char* testName = "process name can come from path";
    WindowCandidate candidate = Candidate(4, L"SharedWindow", L"", L"C:\\Music\\music.exe");

    TargetQuery query;
    query.windowClass = L"SharedWindow";
    query.processName = L"music.exe";
    REQUIRE(ScoreTargetCandidate(candidate, query) >= 0);
}

void TestOptionalProcessPathIsExact() {
    const char* testName = "optional process path is exact";
    const WindowCandidate candidate = Candidate(5, L"SharedWindow", L"music.exe", L"C:\\Music\\music.exe");

    TargetQuery query;
    query.windowClass = L"SharedWindow";
    query.processPath = L"C:\\Other\\music.exe";
    REQUIRE(ScoreTargetCandidate(candidate, query) < 0);

    query.processPath = L"C:\\Music\\music.exe";
    REQUIRE(ScoreTargetCandidate(candidate, query) >= 0);
}

void TestMatchingIsCaseInsensitive() {
    const char* testName = "matching is case insensitive";
    const WindowCandidate candidate = Candidate(6, L"EditorWindow", L"Editor.EXE", L"C:\\Program Files\\Editor\\Editor.EXE");

    TargetQuery query;
    query.windowClass = L"editorwindow";
    query.processName = L"EDITOR.exe";
    query.processPath = L"c:\\program files\\editor\\editor.exe";

    REQUIRE(ScoreTargetCandidate(candidate, query) >= 0);
}

void TestMoreSpecificQueriesScoreHigher() {
    const char* testName = "more specific queries score higher";
    const WindowCandidate candidate = Candidate(7, L"EditorWindow", L"editor.exe", L"C:\\Editor\\editor.exe");

    TargetQuery classOnly;
    classOnly.windowClass = L"EditorWindow";

    TargetQuery withName = classOnly;
    withName.processName = L"editor.exe";

    TargetQuery withPath = withName;
    withPath.processPath = L"C:\\Editor\\editor.exe";

    REQUIRE(ScoreTargetCandidate(candidate, withName) > ScoreTargetCandidate(candidate, classOnly));
    REQUIRE(ScoreTargetCandidate(candidate, withPath) > ScoreTargetCandidate(candidate, withName));
}

void TestResolutionIsDeterministic() {
    const char* testName = "resolution is deterministic";
    const WindowCandidate higherHandle = Candidate(40, L"EditorWindow", L"editor.exe", L"C:\\Editor\\editor.exe");
    const WindowCandidate lowerHandle = Candidate(10, L"EditorWindow", L"editor.exe", L"C:\\Editor\\editor.exe");
    std::vector<WindowCandidate> candidates;
    candidates.push_back(higherHandle);
    candidates.push_back(lowerHandle);

    TargetQuery query;
    query.windowClass = L"EditorWindow";
    query.processName = L"editor.exe";

    WindowCandidate resolved;
    REQUIRE(ResolveTarget(candidates, query, &resolved));
    REQUIRE(resolved.handle == lowerHandle.handle);

    std::reverse(candidates.begin(), candidates.end());
    REQUIRE(ResolveTarget(candidates, query, &resolved));
    REQUIRE(resolved.handle == lowerHandle.handle);
}

void TestEmptyInputsDoNotResolve() {
    const char* testName = "empty inputs do not resolve";
    const std::vector<WindowCandidate> emptyCandidates;
    TargetQuery query;
    query.windowClass = L"EditorWindow";
    WindowCandidate resolved;

    REQUIRE(!ResolveTarget(emptyCandidates, query, &resolved));

    std::vector<WindowCandidate> candidates;
    candidates.push_back(Candidate(8, L"EditorWindow", L"editor.exe", L"C:\\Editor\\editor.exe"));
    query.windowClass.clear();
    REQUIRE(ScoreTargetCandidate(candidates[0], query) < 0);
    REQUIRE(!ResolveTarget(candidates, query, &resolved));
    REQUIRE(!ResolveTarget(candidates, query, NULL));
}

void TestWindowsCommandLineQuoting() {
    const char* testName = "Windows command line quoting";
    std::vector<std::wstring> arguments;
    arguments.push_back(L"plain");
    arguments.push_back(L"two words");
    arguments.push_back(L"");
    arguments.push_back(L"ends with slash\\");
    arguments.push_back(L"embedded\"quote");

    const std::wstring commandLine = BuildWindowsCommandLine(L"C:\\Program Files\\App\\app.exe", arguments);
    REQUIRE(commandLine == L"\"C:\\Program Files\\App\\app.exe\" plain \"two words\" \"\" \"ends with slash\\\\\" \"embedded\\\"quote\"");
    REQUIRE(BuildWindowsCommandLine(L"", arguments).empty());
}

void TestInvalidLaunchDoesNotStartProcess() {
    const char* testName = "invalid launch does not start process";
    LaunchedProcess process = LaunchApplication(L"");

    REQUIRE(!process.started());
    REQUIRE(process.processId() == 0);
    REQUIRE(process.nativeProcessHandle() == 0);
    REQUIRE(process.errorCode() != 0);
    REQUIRE(process.releaseProcessHandle() == 0);
}

void TestEnumerationSmoke() {
    const char* testName = "enumeration smoke";
    const WindowFilterPolicy policy;
    const std::vector<WindowCandidate> candidates = EnumerateWindows(policy);

    for (std::vector<WindowCandidate>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
        REQUIRE(it->handle != 0);
        REQUIRE(IsWindowCandidateAllowed(*it, policy));
    }

    std::cout << "Enumerated " << candidates.size() << " eligible top-level windows.\n";
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"default filtering policy", TestDefaultFilteringPolicy},
        {"filtering policy is configurable", TestFilteringPolicyIsConfigurable},
        {"exact class is required", TestExactClassIsRequired},
        {"optional process name is exact", TestOptionalProcessNameIsExact},
        {"process name can come from path", TestProcessNameCanComeFromPath},
        {"optional process path is exact", TestOptionalProcessPathIsExact},
        {"matching is case insensitive", TestMatchingIsCaseInsensitive},
        {"more specific queries score higher", TestMoreSpecificQueriesScoreHigher},
        {"resolution is deterministic", TestResolutionIsDeterministic},
        {"empty inputs do not resolve", TestEmptyInputsDoNotResolve},
        {"Windows command line quoting", TestWindowsCommandLineQuoting},
        {"invalid launch does not start process", TestInvalidLaunchDoesNotStartProcess},
        {"enumeration smoke", TestEnumerationSmoke},
    };

    for (const TestCase& test : tests) {
        test.run();
        std::cout << "PASS: " << test.name << '\n';
    }

    std::cout << "All Windows target tests passed (13/13).\n";
    return 0;
}
