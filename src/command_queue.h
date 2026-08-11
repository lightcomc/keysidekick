#ifndef KEYBOARD_ROUTER_COMMAND_QUEUE_H
#define KEYBOARD_ROUTER_COMMAND_QUEUE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "mingw_threading.h"
using std::mutex;
using std::lock_guard;
using std::unique_lock;
using std::condition_variable;

namespace command_queue {

template <typename Payload, typename Result, typename Error = std::string>
class CommandQueue {
public:
    typedef std::uint64_t Id;

    enum Status {
        Pending,
        Succeeded,
        Failed
    };

    enum RejectionReason {
        NotRejected,
        QueueFull,
        QueueShutdown
    };

private:
    struct State {
        template <typename PayloadValue>
        State(Id commandId, PayloadValue&& commandPayload)
            : id(commandId),
              payload(std::forward<PayloadValue>(commandPayload)),
              completionStatus(Pending),
              cancellationRequested(false) {}

        template <typename ResultValue>
        bool completeSuccess(ResultValue&& value) {
            std::shared_ptr<const Result> completedValue =
                std::make_shared<Result>(std::forward<ResultValue>(value));
            {
                lock_guard<::mutex> lock(mtx);
                if (completionStatus != Pending) {
                    return false;
                }
                result = completedValue;
                completionStatus = Succeeded;
            }
            completionChanged.notify_all();
            return true;
        }

        template <typename ErrorValue>
        bool completeError(ErrorValue&& value) {
            std::shared_ptr<const Error> completedError =
                std::make_shared<Error>(std::forward<ErrorValue>(value));
            {
                lock_guard<::mutex> lock(mtx);
                if (completionStatus != Pending) {
                    return false;
                }
                error = completedError;
                completionStatus = Failed;
            }
            completionChanged.notify_all();
            return true;
        }

        const Id id;
        const Payload payload;
        mutable ::mutex mtx;
        ::condition_variable completionChanged;
        Status completionStatus;
        bool cancellationRequested;
        std::shared_ptr<const Result> result;
        std::shared_ptr<const Error> error;
    };

public:
    class Outcome {
    public:
        Outcome() : completionStatus(Pending) {}

        Status status() const {
            return completionStatus;
        }

        bool completed() const {
            return completionStatus != Pending;
        }

        bool succeeded() const {
            return completionStatus == Succeeded;
        }

        bool failed() const {
            return completionStatus == Failed;
        }

        const Result& value() const {
            if (!succeeded() || !result) {
                throw std::logic_error("command has no successful result");
            }
            return *result;
        }

        const Error& error() const {
            if (!failed() || !errorValue) {
                throw std::logic_error("command has no error result");
            }
            return *errorValue;
        }

    public:
        Outcome(Status statusValue,
                const std::shared_ptr<const Result>& resultValue,
                const std::shared_ptr<const Error>& commandError)
            : completionStatus(statusValue),
              result(resultValue),
              errorValue(commandError) {}

        Status completionStatus;
        std::shared_ptr<const Result> result;
        std::shared_ptr<const Error> errorValue;

        friend class Request;
    };

    class Request {
    public:
        Request() : rejection(NotRejected) {}

        bool accepted() const {
            return static_cast<bool>(state);
        }

        Id id() const {
            return state ? state->id : 0;
        }

        RejectionReason rejectionReason() const {
            return rejection;
        }

        Status status() const {
            if (!state) {
                return Pending;
            }
            lock_guard<::mutex> lock(state->mtx);
            return state->completionStatus;
        }

        bool requestCancellation() const {
            if (!state) {
                return false;
            }
            lock_guard<::mutex> lock(state->mtx);
            if (state->completionStatus != Pending || state->cancellationRequested) {
                return false;
            }
            state->cancellationRequested = true;
            return true;
        }

        bool isCancellationRequested() const {
            if (!state) {
                return false;
            }
            lock_guard<::mutex> lock(state->mtx);
            return state->cancellationRequested;
        }

        template <typename Rep, typename Period>
        bool waitFor(const std::chrono::duration<Rep, Period>& timeout,
                     Outcome& outcome) const {
            if (!state) {
                return false;
            }
            std::shared_ptr<State> sharedState = state;
            unique_lock<::mutex> lock(sharedState->mtx);
            if (!sharedState->completionChanged.wait_for(
                    lock,
                    timeout,
                    [sharedState]() {
                        return sharedState->completionStatus != Pending;
                    })) {
                return false;
            }
            outcome = Outcome(sharedState->completionStatus,
                              sharedState->result,
                              sharedState->error);
            return true;
        }

        bool wait(Outcome& outcome) const {
            if (!state) {
                return false;
            }
            std::shared_ptr<State> sharedState = state;
            unique_lock<::mutex> lock(sharedState->mtx);
            sharedState->completionChanged.wait(
                lock,
                [sharedState]() {
                    return sharedState->completionStatus != Pending;
                });
            outcome = Outcome(sharedState->completionStatus,
                              sharedState->result,
                              sharedState->error);
            return true;
        }

    public:
        Request(const std::shared_ptr<State>& requestState,
                RejectionReason rejectionReason)
            : state(requestState), rejection(rejectionReason) {}

        std::shared_ptr<State> state;
        RejectionReason rejection;

        friend class CommandQueue;
    };

    class Command {
    public:
        Command() {}

        bool valid() const {
            return static_cast<bool>(state);
        }

        Id id() const {
            return state ? state->id : 0;
        }

        const Payload& payload() const {
            if (!state) {
                throw std::logic_error("invalid command has no payload");
            }
            return state->payload;
        }

        bool isCancellationRequested() const {
            if (!state) {
                return false;
            }
            lock_guard<::mutex> lock(state->mtx);
            return state->cancellationRequested;
        }

        bool fulfillSuccess(const Result& value) const {
            return state && state->completeSuccess(value);
        }

        bool fulfillSuccess(Result&& value) const {
            return state && state->completeSuccess(std::move(value));
        }

        bool fulfillError(const Error& value) const {
            return state && state->completeError(value);
        }

        bool fulfillError(Error&& value) const {
            return state && state->completeError(std::move(value));
        }

    public:
        explicit Command(const std::shared_ptr<State>& commandState)
            : state(commandState) {}

        std::shared_ptr<State> state;

        friend class CommandQueue;
    };

    explicit CommandQueue(std::size_t maximumCapacity)
        : maximumCapacity_(maximumCapacity), shutdown_(false), nextId_(1) {
        if (maximumCapacity_ == 0) {
            throw std::invalid_argument("command queue capacity must be positive");
        }
    }

    ~CommandQueue() {
        shutdown();
    }

    Request enqueue(const Payload& payload) {
        return enqueueValue(payload);
    }

    Request enqueue(Payload&& payload) {
        return enqueueValue(std::move(payload));
    }

    bool tryPop(Command& command) {
        lock_guard<::mutex> lock(mutex_);
        if (commands_.empty()) {
            return false;
        }
        command = Command(commands_.front());
        commands_.pop_front();
        return true;
    }

    bool waitPop(Command& command) {
        unique_lock<::mutex> lock(mutex_);
        queueChanged_.wait(
            lock,
            [this]() {
                return shutdown_ || !commands_.empty();
            });
        if (commands_.empty()) {
            return false;
        }
        command = Command(commands_.front());
        commands_.pop_front();
        return true;
    }

    void shutdown() {
        {
            lock_guard<::mutex> lock(mutex_);
            shutdown_ = true;
        }
        queueChanged_.notify_all();
    }

    bool isShutdown() const {
        lock_guard<::mutex> lock(mutex_);
        return shutdown_;
    }

    std::size_t size() const {
        lock_guard<::mutex> lock(mutex_);
        return commands_.size();
    }

    std::size_t capacity() const {
        return maximumCapacity_;
    }

private:
    template <typename PayloadValue>
    Request enqueueValue(PayloadValue&& payload) {
        std::shared_ptr<State> requestState;
        {
            lock_guard<::mutex> lock(mutex_);
            if (shutdown_) {
                return Request(std::shared_ptr<State>(), QueueShutdown);
            }
            if (commands_.size() >= maximumCapacity_) {
                return Request(std::shared_ptr<State>(), QueueFull);
            }
            if (nextId_ == 0 || nextId_ == std::numeric_limits<Id>::max()) {
                throw std::overflow_error("command id space exhausted");
            }
            requestState = std::make_shared<State>(
                nextId_, std::forward<PayloadValue>(payload));
            ++nextId_;
            commands_.push_back(requestState);
        }
        queueChanged_.notify_one();
        return Request(requestState, NotRejected);
    }

    CommandQueue(const CommandQueue&);
    CommandQueue& operator=(const CommandQueue&);

    const std::size_t maximumCapacity_;
    mutable ::mutex mutex_;
    ::condition_variable queueChanged_;
    std::deque<std::shared_ptr<State> > commands_;
    bool shutdown_;
    Id nextId_;
};

}

#endif
