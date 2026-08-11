// Action parser tests: action string ↔ typed Action round-trip
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/action_parser.h"

using keysidekick::Action;
using keysidekick::ActionType;
using keysidekick::action_parser::ParseActionString;
using keysidekick::action_parser::SerializeAction;
using keysidekick::action_parser::ClassifyAction;
using keysidekick::action_parser::DescribeAction;

static int g_tests = 0, g_failed = 0;

#define CHECK(cond) do { ++g_tests; if(!(cond)){std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);++g_failed;} } while(0)

static void TestParseSwitchProfile() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("!switch:basic", &a));
    CHECK(a.type == ActionType::SwitchProfile);
    CHECK(a.profileId == "basic");
}

static void TestParseToggle() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("!toggle:aimp", &a));
    CHECK(a.type == ActionType::ToggleProfile);
    CHECK(a.profileId == "aimp");
}

static void TestParseLaunch() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("!launch:C:\\AIMP\\AIMP.exe", &a));
    CHECK(a.type == ActionType::LaunchApplication);
    CHECK(a.applicationId == "C:\\AIMP\\AIMP.exe");
}

static void TestParseSendToApp() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("!app:Spotify:{Media_Next_Track}", &a));
    CHECK(a.type == ActionType::SendToApplication);
    CHECK(a.applicationId == "Spotify");
    CHECK(a.steps.size() == 1);
    CHECK(a.steps[0].profileId == "{Media_Next_Track}");
}

static void TestParseMediaKey() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("{Media_Play_Pause}", &a));
    CHECK(a.type == ActionType::SendMediaKey);
    CHECK(a.profileId == "{Media_Play_Pause}");
}

static void TestParseRegularKey() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("{F1}", &a));
    CHECK(a.type == ActionType::SendKey);
    CHECK(a.profileId == "{F1}");
}

static void TestParsePlainText() {
    Action a = Action::sendKey(0);
    CHECK(ParseActionString("1", &a));
    CHECK(a.type == ActionType::SendKey);
    CHECK(a.profileId == "1");
}

static void TestSerializeRoundTrip() {
    const char* cases[] = {
        "!switch:basic",
        "!toggle:aimp",
        "!launch:C:\\AIMP\\AIMP.exe",
        "{F1}",
        "{Media_Play_Pause}",
        "1",
        "w",
        "!app:Spotify:{Media_Next_Track}",
    };
    for (const char* orig : cases) {
        Action a = Action::sendKey(0);
        CHECK(ParseActionString(orig, &a));
        std::string serialized = SerializeAction(a);
        CHECK(serialized == orig);
    }
}

static void TestClassify() {
    CHECK(ClassifyAction("!switch:basic") == "switch");
    CHECK(ClassifyAction("!toggle:aimp") == "toggle");
    CHECK(ClassifyAction("!launch:C:\\app.exe") == "launch");
    CHECK(ClassifyAction("!app:Spotify:{Media_Next_Track}") == "multi-app");
    CHECK(ClassifyAction("{F1}") == "key");
    CHECK(ClassifyAction("{Media_Play_Pause}") == "media");
    CHECK(ClassifyAction("1") == "key");
}

static void TestDescribe() {
    CHECK(DescribeAction("!switch:basic") == "Switch to basic");
    CHECK(DescribeAction("!toggle:aimp") == "Toggle aimp");
    CHECK(DescribeAction("!launch:C:\\AIMP\\AIMP.exe") == "Launch AIMP");
    CHECK(DescribeAction("!app:Spotify:{Media_Next_Track}") == "{Media_Next_Track} → Spotify");
    CHECK(DescribeAction("{F1}") == "{F1}");
}

int main() {
    TestParseSwitchProfile();
    TestParseToggle();
    TestParseLaunch();
    TestParseSendToApp();
    TestParseMediaKey();
    TestParseRegularKey();
    TestParsePlainText();
    TestSerializeRoundTrip();
    TestClassify();
    TestDescribe();

    if (g_failed == 0) {
        std::printf("All action_parser tests passed (%d checks)\n", g_tests);
        return 0;
    }
    std::printf("%d/%d checks FAILED\n", g_failed, g_tests);
    return 1;
}
