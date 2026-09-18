// mingw_threading_tests.cpp — regression cover for the Win32 threading shim
// (src/mingw_threading.h) used by command_queue/runtime_state and their suites.
//
// The shim's `thread` used to be copyable: `std::vector<std::thread>::push_back`
// copied the raw HANDLE, the temporary's destructor closed it, and join() then
// waited on a closed handle — returning immediately, before the thread had run.
// Measured before the fix: 16 threads incrementing one counter -> 0..9 after all
// joins. Every concurrency assertion built on join() was therefore meaningless.

#include "../src/mingw_threading.h"

#include <atomic>
#include <cstdio>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void Check(const char* testName, bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s: %s\n", testName, what);
    }
}

// Mirrors the real usage pattern: threads moved into a vector, then joined.
void TestJoinWaitsForEveryThread() {
    const char* testName = "join waits for every thread";
    std::atomic<int> counter(0);
    std::vector<std::thread> threads;
    for (int index = 0; index < 16; ++index) {
        threads.push_back(std::thread([&counter]() { ++counter; }));
    }
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index].join();
    }
    Check(testName, counter.load() == 16, "all 16 increments visible after join");
}

void TestMovedThreadKeepsJoinability() {
    const char* testName = "move transfers ownership";
    std::atomic<bool> ran(false);
    std::thread first([&ran]() { ran = true; });
    Check(testName, first.joinable(), "constructed thread is joinable");

    std::thread second(std::move(first));
    Check(testName, !first.joinable(), "moved-from thread is not joinable");
    Check(testName, second.joinable(), "moved-to thread owns the handle");

    second.join();
    Check(testName, ran.load(), "work completed before join returned");
    Check(testName, !second.joinable(), "joined thread is not joinable");
}

void TestMoveAssignmentClosesPreviousHandle() {
    const char* testName = "move assignment";
    std::atomic<int> counter(0);
    std::thread worker([&counter]() { ++counter; });
    std::thread other;
    other = std::move(worker);
    Check(testName, !worker.joinable(), "source released on move-assign");
    other.join();
    Check(testName, counter.load() == 1, "the surviving handle joins the work");
}

void TestConditionVariableHandshake() {
    const char* testName = "condition variable handshake";
    std::mutex mtx;
    std::condition_variable cv;
    int value = 0;
    bool ready = false;

    std::thread producer([&mtx, &cv, &value, &ready]() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            value = 42;
            ready = true;
        }
        cv.notify_all();
    });

    bool saw = false;
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&ready]() { return ready; });
        saw = (value == 42);
    }
    producer.join();
    Check(testName, saw, "consumer observed the produced value");
}

}  // namespace

int main() {
    TestJoinWaitsForEveryThread();
    TestMovedThreadKeepsJoinability();
    TestMoveAssignmentClosesPreviousHandle();
    TestConditionVariableHandshake();

    std::printf("mingw_threading: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
