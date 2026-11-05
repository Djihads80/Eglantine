// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_fence_wake.h"

using Service::Nx2::FenceWake;

TEST_CASE("Capture fence timeout accepts only the diagnostic bounded range",
          "[core][nx2_fence_wake]") {
    using Service::Nx2::CaptureFenceTimeoutMilliseconds;
    CHECK(CaptureFenceTimeoutMilliseconds("") == 4000);
    CHECK(CaptureFenceTimeoutMilliseconds("4000") == 4000);
    CHECK(CaptureFenceTimeoutMilliseconds("30000") == 30000);
    CHECK(CaptureFenceTimeoutMilliseconds("120000") == 120000);
    for (const auto invalid : {"3999", "120001", "-1", "+5000", "5000ms", " 5000", "5000 ",
                               "invalid", "999999999999999999999999999999"}) {
        CHECK(CaptureFenceTimeoutMilliseconds(invalid) == 4000);
    }
}

TEST_CASE("Present fence wake retains an immediate signal and ignores duplicates",
          "[core][nx2_fence_wake]") {
    unsigned signals{};
    const auto wake = std::make_shared<FenceWake>([&] { ++signals; });
    const auto callback = FenceWake::Callback(wake);
    callback(); // RegisterGuestAction may invoke inline, before the kernel wait begins.
    CHECK(signals == 1);
    callback();
    wake->Cancel();
    callback();
    CHECK(signals == 1);
}

TEST_CASE("A cancelled or expired present callback cannot wake a later frame",
          "[core][nx2_fence_wake]") {
    unsigned signals{};
    auto first = std::make_shared<FenceWake>([&] { ++signals; });
    const auto old_callback = FenceWake::Callback(first);
    first->Cancel(); // Timeout or shutdown, before its event is closed/reused.
    old_callback();
    CHECK(signals == 0);
    const std::weak_ptr old_state{first};
    first.reset();
    CHECK(old_state.expired()); // A pending syncpoint action does not retain the event owner.
    const auto next = std::make_shared<FenceWake>([&] { ++signals; });
    const auto new_callback = FenceWake::Callback(next);
    old_callback();
    CHECK(signals == 0);
    new_callback();
    CHECK(signals == 1);
}

TEST_CASE("Present wake cancellation waits for an in-flight event signal",
          "[core][nx2_fence_wake]") {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{}, release{}, signaled{};
    std::atomic<bool> cancel_started{}, cancelled{};
    const auto wake = std::make_shared<FenceWake>([&] {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        signaled = true;
    });
    const auto callback = FenceWake::Callback(wake);
    std::jthread signal_thread([&] { callback(); });
    bool began;
    {
        std::unique_lock lock{mutex};
        began = cv.wait_for(lock, 5s, [&] { return entered; });
    }
    std::jthread cancel_thread([&] {
        cancel_started.store(true, std::memory_order_release);
        wake->Cancel();
        std::scoped_lock lock{mutex};
        cancelled.store(true, std::memory_order_release);
        cv.notify_all();
    });
    while (!cancel_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool premature;
    {
        std::unique_lock lock{mutex};
        premature = cv.wait_for(lock, 50ms, [&] {
            return cancelled.load(std::memory_order_acquire);
        });
        release = true;
        cv.notify_all();
    }
    signal_thread.join();
    cancel_thread.join();
    CHECK(began);
    CHECK_FALSE(premature);
    CHECK(signaled);
    CHECK(cancelled.load(std::memory_order_acquire));
    callback(); // Event may now be closed: the stale callback has no signal operation.
}
