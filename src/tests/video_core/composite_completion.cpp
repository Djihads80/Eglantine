// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/composite_completion.h"

namespace {
using namespace std::chrono_literals;

struct Observed {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<unsigned, bool>> values;
    void Add(unsigned id, bool completed) {
        std::scoped_lock lock{mutex};
        values.emplace_back(id, completed);
        cv.notify_all();
    }
    bool WaitFor(size_t count) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, 2s, [&] { return values.size() >= count; });
    }
};
} // namespace

TEST_CASE("Composite release requires the submitted GPU tick, not a ready render fence",
          "[composite_completion]") {
    Observed observed;
    std::atomic<unsigned> gpu_tick{6};
    std::atomic<unsigned> probes{};
    VideoCore::CompositeCompletionQueue queue;
    const auto caller = std::this_thread::get_id();
    std::thread::id completing_thread;
    constexpr unsigned submitted_tick = 7;
    REQUIRE(queue.Enqueue(
        [&] {
            probes.fetch_add(1);
            return gpu_tick.load() >= submitted_tick;
        },
        [&](bool completed) {
            completing_thread = std::this_thread::get_id();
            observed.Add(0, completed);
        }));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (probes.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(probes.load() > 0);
    {
        std::scoped_lock lock{observed.mutex};
        CHECK(observed.values.empty());
    }
    // The real submission may contain only the compositor blit, with no cache downloads.
    // It still releases nothing until its hardware completion tick advances.
    gpu_tick.store(submitted_tick);
    CHECK(observed.WaitFor(1));
    queue.Stop();
    REQUIRE(observed.values.size() == 1);
    CHECK(observed.values[0].second);
    CHECK(completing_thread != caller);
}

TEST_CASE("An unsatisfied composite input does not block another ready layer",
          "[composite_completion]") {
    Observed observed;
    VideoCore::CompositeCompletionQueue queue;
    REQUIRE(queue.Enqueue([] { return false; }, [&](bool ok) { observed.Add(1, ok); }));
    REQUIRE(queue.Enqueue([] { return true; }, [&](bool ok) { observed.Add(2, ok); }));
    CHECK(observed.WaitFor(1));
    queue.Stop();
    REQUIRE(observed.values.size() == 2);
    CHECK((observed.values[0] == std::pair<unsigned, bool>{2, true}));
    CHECK((observed.values[1] == std::pair<unsigned, bool>{1, false}));
}

TEST_CASE("Composite dispatch is FIFO per layer while another layer can progress",
          "[composite_completion]") {
    Observed observed;
    std::atomic<bool> first_ready{};
    VideoCore::CompositeCompletionQueue queue;
    REQUIRE(queue.Enqueue([&] { return first_ready.load(); },
                          [&](bool ok) { observed.Add(1, ok); }, 100));
    REQUIRE(queue.Enqueue([] { return true; }, [&](bool ok) { observed.Add(2, ok); }, 100));
    REQUIRE(queue.Enqueue([] { return true; }, [&](bool ok) { observed.Add(3, ok); }, 200));
    CHECK(observed.WaitFor(1));
    {
        std::scoped_lock lock{observed.mutex};
        REQUIRE(observed.values.size() == 1);
        CHECK((observed.values[0] == std::pair<unsigned, bool>{3, true}));
    }
    first_ready.store(true);
    CHECK(observed.WaitFor(3));
    queue.Stop();
    REQUIRE(observed.values.size() == 3);
    CHECK((observed.values[1] == std::pair<unsigned, bool>{1, true}));
    CHECK((observed.values[2] == std::pair<unsigned, bool>{2, true}));
}

TEST_CASE("Composite completion token cancels dropped requests and completes only once",
          "[composite_completion]") {
    Observed observed;
    {
        const auto dropped = std::make_shared<VideoCore::CompositeCompletion>(
            [&](bool ok) { observed.Add(1, ok); });
    }
    {
        const auto completed = std::make_shared<VideoCore::CompositeCompletion>(
            [&](bool ok) { observed.Add(2, ok); });
        completed->Complete(true);
        completed->Complete(false);
        completed->Complete(true);
    }
    REQUIRE(observed.values.size() == 2);
    CHECK((observed.values[0] == std::pair<unsigned, bool>{1, false}));
    CHECK((observed.values[1] == std::pair<unsigned, bool>{2, true}));
}

TEST_CASE("Composite probe failure and stopped queue never report completion",
          "[composite_completion]") {
    Observed observed;
    VideoCore::CompositeCompletionQueue queue;
    REQUIRE(queue.Enqueue([]() -> bool { throw std::runtime_error("device lost"); },
                          [&](bool ok) { observed.Add(1, ok); }));
    CHECK(observed.WaitFor(1));
    queue.Stop();
    CHECK_FALSE(queue.Enqueue([] { return true; }, [&](bool ok) { observed.Add(2, ok); }));
    REQUIRE(observed.values.size() == 2);
    CHECK_FALSE(observed.values[0].second);
    CHECK_FALSE(observed.values[1].second);
}

TEST_CASE("Queued composite requests cancel before teardown and outside the request lock",
          "[composite_completion]") {
    std::mutex mutex;
    std::list<std::function<void()>> requests;
    unsigned cancellations{};
    bool unlocked{}, success{};
    auto token = std::make_shared<VideoCore::CompositeCompletion>([&](bool completed) {
        ++cancellations;
        success = completed;
        unlocked = mutex.try_lock();
        if (unlocked) {
            mutex.unlock();
        }
    });
    requests.emplace_back([token] { token->Complete(true); });
    token.reset();
    CHECK(cancellations == 0);
    VideoCore::DiscardPendingSyncRequests(mutex, requests);
    CHECK(requests.empty());
    CHECK(cancellations == 1);
    CHECK_FALSE(success);
    CHECK(unlocked);
    VideoCore::DiscardPendingSyncRequests(mutex, requests);
    CHECK(cancellations == 1);
}

TEST_CASE("Stopping composite completions waits for an in-flight publication",
          "[composite_completion]") {
    VideoCore::CompositeCompletionQueue queue;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{}, release{}, published{};
    std::atomic<bool> stopping{}, stopped{};
    REQUIRE(queue.Enqueue([] { return true; }, [&](bool completed) {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        published = completed;
    }));
    {
        std::unique_lock lock{mutex};
        if (!cv.wait_for(lock, 2s, [&] { return entered; })) {
            release = true;
            cv.notify_all();
            FAIL("completion worker did not invoke its ready callback");
        }
    }
    std::jthread stop_thread([&] {
        stopping.store(true);
        queue.Stop();
        stopped.store(true);
    });
    while (!stopping.load()) {
        std::this_thread::yield();
    }
    CHECK_FALSE(stopped.load());
    {
        std::scoped_lock lock{mutex};
        release = true;
        cv.notify_all();
    }
    stop_thread.join();
    CHECK(stopped.load());
    CHECK(published);
}
