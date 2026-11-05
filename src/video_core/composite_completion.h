// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "common/polyfill_thread.h"

namespace VideoCore {

// Owned requests can be discarded before a compositor receives them (shutdown, rejected
// inputs, exceptions). Their last owner reports failure, never successful image release.
// The callback must not throw; it must hold only lifecycle-safe service/ticket state.
class CompositeCompletion {
public:
    using Callback = std::function<void(bool completed)>;

    explicit CompositeCompletion(Callback callback_) : callback{std::move(callback_)} {}
    ~CompositeCompletion() {
        Complete(false);
    }

    void Complete(bool completed) noexcept {
        Callback invoke;
        {
            std::scoped_lock lock{mutex};
            invoke = std::exchange(callback, {});
        }
        if (invoke) {
            invoke(completed);
        }
    }

private:
    std::mutex mutex;
    Callback callback;
};

// GPU shutdown discards requests that never reached the renderer before services close.
// Destruction can cancel owned tokens and call services, so it must happen outside this lock.
inline void DiscardPendingSyncRequests(std::mutex& mutex,
                                      std::list<std::function<void()>>& requests) {
    std::list<std::function<void()>> abandoned;
    {
        std::scoped_lock lock{mutex};
        abandoned.swap(requests);
    }
}

// A lazy worker for bounded, nonblocking readiness probes. It never holds its queue lock
// while probing or invoking a callback. Each accepted callback runs once, on this worker;
// false means cancellation/probe failure, not completion. Nonzero ordering keys retain FIFO
// dispatch within a layer. A stalled layer does not block jobs with another ordering key.
// Stop must run on an owning thread, not inside a callback. It waits for in-flight callbacks
// and cancels accepted work before returning. Producers must stop before destroying the queue.
class CompositeCompletionQueue {
public:
    using Ready = std::function<bool()>;
    using Callback = CompositeCompletion::Callback;

    ~CompositeCompletionQueue() {
        Stop();
    }

    bool Enqueue(Ready ready, Callback callback, std::uint64_t ordering_key = 0) {
        {
            std::scoped_lock lock{mutex};
            if (accepting) {
                pending.push_back(Job{std::move(ready), std::move(callback), ordering_key});
                if (!worker.joinable()) {
                    worker = std::jthread([this](std::stop_token stop) { Run(stop); });
                }
                wake.notify_one();
                return true;
            }
        }
        // Rejection is synchronous and outside the lock. Callers must not hold service locks.
        callback(false);
        return false;
    }

    void Stop() {
        std::scoped_lock stop_lock{stop_mutex};
        {
            std::scoped_lock lock{mutex};
            accepting = false;
            if (worker.joinable()) {
                worker.request_stop();
            }
            wake.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

private:
    struct Job {
        Ready ready;
        Callback callback;
        std::uint64_t ordering_key;
    };

    void TakePending(std::vector<Job>& waiting) {
        while (!pending.empty()) {
            waiting.push_back(std::move(pending.front()));
            pending.pop_front();
        }
    }

    void Run(std::stop_token stop) {
        std::vector<Job> waiting;
        std::vector<std::uint64_t> blocked_keys;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock{mutex};
                if (waiting.empty()) {
                    wake.wait(lock, stop, [this] { return !pending.empty(); });
                }
                TakePending(waiting);
            }
            blocked_keys.clear();
            for (auto it = waiting.begin(); it != waiting.end() && !stop.stop_requested();) {
                if (it->ordering_key != 0 &&
                    std::find(blocked_keys.begin(), blocked_keys.end(), it->ordering_key) !=
                        blocked_keys.end()) {
                    ++it;
                    continue;
                }
                bool ready = false;
                bool failed = false;
                try {
                    ready = it->ready();
                } catch (...) {
                    // The owner reports device/probe errors; no failed probe releases an image.
                    failed = true;
                }
                if (!ready && !failed) {
                    if (it->ordering_key != 0) {
                        blocked_keys.push_back(it->ordering_key);
                    }
                    ++it;
                    continue;
                }
                Job done = std::move(*it);
                it = waiting.erase(it);
                done.callback(ready && !failed && !stop.stop_requested());
            }
            if (!waiting.empty() && !stop.stop_requested()) {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::milliseconds{1},
                              [this] { return !pending.empty(); });
            }
        }
        {
            std::scoped_lock lock{mutex};
            TakePending(waiting);
        }
        for (auto& job : waiting) {
            job.callback(false);
        }
    }

    std::mutex mutex;
    std::mutex stop_mutex;
    std::condition_variable_any wake;
    std::deque<Job> pending;
    bool accepting{true};
    std::jthread worker;
};

} // namespace VideoCore
