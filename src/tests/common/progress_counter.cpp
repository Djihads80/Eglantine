// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/progress_counter.h"

using namespace std::chrono_literals;
using Common::ProgressCounter;
using WaitResult = ProgressCounter::WaitResult;

namespace {
// Generous bound so a broken wakeup fails the test instead of hanging it; never a timing check.
constexpr auto Bound = 20s;

auto Deadline() {
    return std::chrono::steady_clock::now() + Bound;
}
} // Anonymous namespace

TEST_CASE("ProgressCounter reached targets return at once", "[common][progress]") {
    ProgressCounter counter{5};
    CHECK(counter.WaitFor(0) == WaitResult::Reached);
    CHECK(counter.WaitFor(5) == WaitResult::Reached);
    CHECK(counter.Reached(5));
    CHECK_FALSE(counter.Reached(6));
    CHECK(counter.BlockCount() == 0);
    CHECK(counter.LowestTarget() == ProgressCounter::NoTarget);
}

TEST_CASE("ProgressCounter wakes blocked waiters only once their target is reached",
          "[common][progress]") {
    ProgressCounter counter;
    constexpr u64 Waiters = 6;
    std::vector<std::jthread> threads;
    std::array<std::atomic<u64>, Waiters> seen_at_return{};
    std::atomic<u32> done{};
    for (u64 i = 0; i < Waiters; ++i) {
        threads.emplace_back([&, i] {
            const u64 target = (i + 1) * 10;
            // No spin: always the blocking path.
            if (counter.WaitUntil(target, Deadline(), 0ns) == WaitResult::Reached) {
                seen_at_return[i] = counter.Value();
                done.fetch_add(1);
            }
        });
    }
    // The producer steps through every value; each waiter must see its own target reached.
    for (u64 value = 1; value <= Waiters * 10; ++value) {
        counter.Advance(value);
        if (value % 7 == 0) {
            std::this_thread::yield();
        }
    }
    threads.clear();
    CHECK(done == Waiters);
    for (u64 i = 0; i < Waiters; ++i) {
        CHECK(seen_at_return[i] >= (i + 1) * 10);
    }
    CHECK(counter.LowestTarget() == ProgressCounter::NoTarget);
}

TEST_CASE("ProgressCounter cancel ends waits for unreached targets", "[common][progress]") {
    ProgressCounter counter;
    std::atomic<int> result{-1};
    std::jthread waiter{[&] { result = static_cast<int>(counter.WaitUntil(100, Deadline())); }};
    while (!counter.HasSleepers()) {
        std::this_thread::yield();
    }
    CHECK(counter.LowestTarget() == 100);
    counter.Advance(50);
    counter.Cancel();
    waiter.join();
    CHECK(result == static_cast<int>(WaitResult::Cancelled));
    // A target already reached still counts as reached after a cancel.
    CHECK(counter.WaitFor(50) == WaitResult::Reached);
    CHECK(counter.WaitFor(51) == WaitResult::Cancelled);
    counter.Reset(0);
    CHECK_FALSE(counter.IsCancelled());
    counter.Advance(3);
    CHECK(counter.WaitFor(3) == WaitResult::Reached);
}

TEST_CASE("ProgressCounter timeouts and abort predicates end waits", "[common][progress]") {
    ProgressCounter counter;
    CHECK(counter.WaitUntil(1, std::chrono::steady_clock::now() + 2ms) == WaitResult::TimedOut);
    std::atomic<bool> abort{};
    std::jthread aborter{[&] {
        std::this_thread::sleep_for(2ms);
        abort = true;
    }};
    CHECK(counter.Wait(1, 0ns, Deadline(), [&] { return abort.load(); }, 1ms) ==
          WaitResult::Cancelled);
    // An abort that comes with the target reached reports it reached.
    counter.Advance(1);
    CHECK(counter.Wait(1, 0ns, Deadline(), [] { return true; }, 1ms) == WaitResult::Reached);
}

TEST_CASE("ProgressCounter competing producers lose no increments or wakeups",
          "[common][progress]") {
    ProgressCounter counter;
    constexpr u64 Producers = 4;
    constexpr u64 PerProducer = 20000;
    std::atomic<bool> go{};
    std::vector<std::jthread> threads;
    std::atomic<u64> reached{};
    // Waiters for staggered targets, mixed spin budgets.
    for (u64 i = 1; i <= 4; ++i) {
        threads.emplace_back([&, i] {
            const u64 target = Producers * PerProducer * i / 4;
            if (counter.WaitUntil(target, Deadline(), i % 2 ? 0ns : 50us) == WaitResult::Reached) {
                reached.fetch_add(1);
            }
        });
    }
    for (u64 p = 0; p < Producers; ++p) {
        threads.emplace_back([&, p] {
            while (!go) {
                std::this_thread::yield();
            }
            for (u64 i = 0; i < PerProducer; ++i) {
                if ((i + p) % 3 == 0) {
                    counter.Increment();
                } else {
                    // AdvanceAtLeast with a stale value must not move the counter back.
                    const u64 seen = counter.Value();
                    counter.AdvanceAtLeast(seen);
                    counter.Increment();
                }
            }
        });
    }
    go = true;
    threads.clear();
    CHECK(counter.Value() == Producers * PerProducer);
    CHECK(reached == 4);
}

TEST_CASE("ProgressCounter ping-pong never loses a wakeup", "[common][progress]") {
    // Two counters handed back and forth one step at a time, blocking every time: a lost
    // wakeup would stall until the bound (and fail), a spurious one would break the order.
    ProgressCounter ping;
    ProgressCounter pong;
    constexpr u64 Rounds = 3000;
    std::atomic<u64> errors{};
    std::jthread other{[&] {
        for (u64 i = 1; i <= Rounds; ++i) {
            if (ping.WaitUntil(i, Deadline(), 0ns) != WaitResult::Reached || ping.Value() != i) {
                errors.fetch_add(1);
            }
            pong.Advance(i);
        }
    }};
    u64 local_errors = 0;
    for (u64 i = 1; i <= Rounds; ++i) {
        ping.Advance(i);
        if (pong.WaitUntil(i, Deadline(), (i & 1) ? 0ns : 5us) != WaitResult::Reached) {
            ++local_errors;
        }
    }
    other.join();
    CHECK(errors == 0);
    CHECK(local_errors == 0);
    CHECK(pong.Value() == Rounds);
}

TEST_CASE("ProgressCounter published values reach blocked waiters at their next poll",
          "[common][progress]") {
    ProgressCounter counter;
    std::atomic<bool> reached{};
    std::jthread waiter{[&] {
        reached = counter.Wait(7, 0ns, Deadline(), [] { return false; }, 1ms) == WaitResult::Reached;
    }};
    while (!counter.HasSleepers()) {
        std::this_thread::yield();
    }
    counter.Publish(7); // no wakeup: the poll finds it
    waiter.join();
    CHECK(reached);
}
