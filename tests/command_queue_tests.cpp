#include "../src/mingw_threading.h"
#include "../src/command_queue.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;

struct ProfileCommand {
    enum Type { SetProfile, Reload };

    ProfileCommand(Type commandType = Reload, const std::string& profileName = std::string())
        : type(commandType), profile(profileName) {}

    Type type;
    std::string profile;
};

struct ProfileResult {
    ProfileResult(bool commandSucceeded = false,
                  const std::string& profileName = std::string())
        : succeeded(commandSucceeded), activeProfile(profileName) {}

    bool succeeded;
    std::string activeProfile;
};

using Queue = command_queue::CommandQueue<ProfileCommand, ProfileResult>;

void testNormalCompletion() {
    Queue queue(4);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::SetProfile, "gaming"));
    assert(request.accepted());
    assert(request.id() != 0);

    std::thread owner([&queue]() {
        Queue::Command command;
        assert(queue.waitPop(command));
        assert(command.id() != 0);
        assert(command.payload().profile == "gaming");
        assert(command.fulfillSuccess(ProfileResult(true, command.payload().profile)));
    });

    Queue::Outcome outcome;
    assert(request.waitFor(milliseconds(1000), outcome));
    assert(outcome.succeeded());
    assert(outcome.value().succeeded);
    assert(outcome.value().activeProfile == "gaming");
    owner.join();
}

void testTimeoutThenLateCompletion() {
    Queue queue(2);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    assert(request.accepted());

    Queue::Outcome outcome;
    assert(!request.waitFor(milliseconds(15), outcome));
    assert(request.status() == Queue::Pending);

    Queue::Command command;
    assert(queue.tryPop(command));
    assert(command.fulfillSuccess(ProfileResult(true, "basic")));
    assert(request.waitFor(milliseconds(100), outcome));
    assert(outcome.succeeded());
    assert(outcome.value().activeProfile == "basic");
}

void testCallerCanReleaseTimedOutRequest() {
    Queue queue(1);
    Queue::Id commandId = 0;
    {
        Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
        assert(request.accepted());
        commandId = request.id();
        Queue::Outcome outcome;
        assert(!request.waitFor(milliseconds(5), outcome));
    }

    Queue::Command command;
    assert(queue.tryPop(command));
    assert(command.id() == commandId);
    assert(command.fulfillSuccess(ProfileResult(true, "late")));
}

void testCancellationFlag() {
    Queue queue(2);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::SetProfile, "work"));
    assert(request.accepted());
    assert(!request.isCancellationRequested());
    assert(request.requestCancellation());
    assert(request.isCancellationRequested());
    assert(!request.requestCancellation());

    Queue::Command command;
    assert(queue.tryPop(command));
    assert(command.isCancellationRequested());
    assert(command.fulfillError("cancelled by caller"));

    Queue::Outcome outcome;
    assert(request.waitFor(milliseconds(100), outcome));
    assert(outcome.failed());
    assert(outcome.error() == "cancelled by caller");
}

void testQueueShutdown() {
    Queue queue(2);
    std::atomic<bool> waiterReturned(false);
    std::atomic<bool> waiterPopped(true);

    std::thread waiter([&queue, &waiterReturned, &waiterPopped]() {
        Queue::Command command;
        waiterPopped.store(queue.waitPop(command));
        waiterReturned.store(true);
    });

    std::this_thread::sleep_for(milliseconds(20));
    queue.shutdown();
    waiter.join();
    assert(waiterReturned.load());
    assert(!waiterPopped.load());
    assert(queue.isShutdown());

    Queue::Request rejected = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    assert(!rejected.accepted());
    assert(rejected.rejectionReason() == Queue::QueueShutdown);

    Queue::Command command;
    assert(!queue.tryPop(command));
    assert(!queue.waitPop(command));
}

void testShutdownLeavesQueuedCommandsDrainable() {
    Queue queue(2);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    assert(request.accepted());
    queue.shutdown();

    Queue::Command command;
    assert(queue.waitPop(command));
    assert(command.fulfillSuccess(ProfileResult(true, "drained")));
    assert(!queue.waitPop(command));

    Queue::Outcome outcome;
    assert(request.waitFor(milliseconds(100), outcome));
    assert(outcome.value().activeProfile == "drained");
}

void testBoundedRejection() {
    Queue queue(1);
    Queue::Request first = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    Queue::Request second = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    assert(first.accepted());
    assert(!second.accepted());
    assert(second.id() == 0);
    assert(second.rejectionReason() == Queue::QueueFull);
    assert(queue.size() == 1);

    Queue::Command command;
    assert(queue.tryPop(command));
    assert(command.fulfillSuccess(ProfileResult(true, "basic")));

    Queue::Request third = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    assert(third.accepted());
}

void testExactlyOnceCompletion() {
    Queue queue(1);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    Queue::Command command;
    assert(queue.tryPop(command));

    assert(command.fulfillSuccess(ProfileResult(true, "first")));
    assert(!command.fulfillSuccess(ProfileResult(true, "second")));
    assert(!command.fulfillError("too late"));

    Queue::Outcome outcome;
    assert(request.waitFor(milliseconds(100), outcome));
    assert(outcome.succeeded());
    assert(outcome.value().activeProfile == "first");
}

void testConcurrentExactlyOnceCompletion() {
    const int completerCount = 16;
    Queue queue(1);
    Queue::Request request = queue.enqueue(ProfileCommand(ProfileCommand::Reload));
    Queue::Command command;
    assert(queue.tryPop(command));

    std::atomic<int> completionWins(0);
    std::vector<std::thread> completers;
    for (int index = 0; index < completerCount; ++index) {
        completers.push_back(std::thread([index, command, &completionWins]() {
            const std::string value = std::to_string(index);
            if (command.fulfillSuccess(ProfileResult(true, value))) {
                ++completionWins;
            }
        }));
    }
    for (std::size_t index = 0; index < completers.size(); ++index) {
        completers[index].join();
    }

    assert(completionWins.load() == 1);
    Queue::Outcome outcome;
    assert(request.waitFor(milliseconds(100), outcome));
    assert(outcome.succeeded());
}

void testMultipleProducers() {
    const int producerCount = 6;
    const int commandsPerProducer = 80;
    const int totalCommands = producerCount * commandsPerProducer;
    Queue queue(static_cast<std::size_t>(totalCommands));

    std::vector<Queue::Request> requests;
    std::mutex requestsMutex;
    std::vector<std::thread> producers;

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.push_back(std::thread([producer, commandsPerProducer,
                                         &queue, &requests, &requestsMutex]() {
            for (int index = 0; index < commandsPerProducer; ++index) {
                const std::string name = std::to_string(producer) + ":" + std::to_string(index);
                Queue::Request request =
                    queue.enqueue(ProfileCommand(ProfileCommand::SetProfile, name));
                assert(request.accepted());
                std::lock_guard<std::mutex> lock(requestsMutex);
                requests.push_back(request);
            }
        }));
    }

    std::vector<Queue::Id> processedIds;
    std::mutex processedMutex;
    std::thread owner([&queue, &processedIds, &processedMutex, totalCommands]() {
        for (int index = 0; index < totalCommands; ++index) {
            Queue::Command command;
            assert(queue.waitPop(command));
            {
                std::lock_guard<std::mutex> lock(processedMutex);
                processedIds.push_back(command.id());
            }
            assert(command.fulfillSuccess(ProfileResult(true, command.payload().profile)));
        }
    });

    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index].join();
    }
    owner.join();

    assert(requests.size() == static_cast<std::size_t>(totalCommands));
    std::set<Queue::Id> requestIds;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        Queue::Outcome outcome;
        assert(requests[index].waitFor(milliseconds(100), outcome));
        assert(outcome.succeeded());
        requestIds.insert(requests[index].id());
    }

    std::set<Queue::Id> processedIdSet(processedIds.begin(), processedIds.end());
    assert(requestIds.size() == static_cast<std::size_t>(totalCommands));
    assert(processedIdSet == requestIds);
}

}  // namespace

int main() {
    testNormalCompletion();
    testTimeoutThenLateCompletion();
    testCallerCanReleaseTimedOutRequest();
    testCancellationFlag();
    testQueueShutdown();
    testShutdownLeavesQueuedCommandsDrainable();
    testBoundedRejection();
    testExactlyOnceCompletion();
    testConcurrentExactlyOnceCompletion();
    testMultipleProducers();
    std::cout << "command_queue_tests: all tests passed\n";
    return 0;
}
