// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "common/event_wait.h"
#include "common/thread.h"

using namespace std::chrono_literals;

TEST_CASE("Event deadlines preserve elapsed time across wakeups", "[common][event]") {
    const auto deadline = Common::EventWaitDeadline(100ns, 2s);
    REQUIRE(deadline == 2s + 100ns);
    CHECK(Common::MwaitxTimeoutCycles(deadline - 100ns, 3'700'000'000) == 0xffffffffU);
    CHECK(Common::MwaitxTimeoutCycles(deadline - (1s + 100ns), 3'700'000'000) ==
          3'700'000'000U);
    CHECK(Common::MwaitxTimeoutCycles(deadline - (2s + 99ns), 3'700'000'000) == 4);
    CHECK(Common::MwaitxTimeoutCycles(deadline - (2s + 100ns), 3'700'000'000) == 0);
    CHECK(Common::MwaitxTimeoutCycles(deadline - (2s + 101ns), 3'700'000'000) == 0);
}

TEST_CASE("Event deadlines saturate and reject nonpositive timeouts", "[common][event]") {
    constexpr auto Max = std::chrono::nanoseconds::max();
    CHECK(Common::EventWaitDeadline(100ns, 0ns) == 100ns);
    CHECK(Common::EventWaitDeadline(100ns, -1ns) == 100ns);
    CHECK(Common::EventWaitDeadline(100ns, std::chrono::nanoseconds::min()) == 100ns);
    CHECK(Common::EventWaitDeadline(100ns, Max) == Max);
    CHECK(Common::EventWaitDeadline(Max - 1ns, 1ns) == Max);
    CHECK(Common::EventWaitDeadline(Max - 1ns, 2ns) == Max);
    CHECK(Common::MwaitxTimeoutCycles(Max, 3'700'000'000) == 0xffffffffU);
    CHECK(Common::MwaitxTimeoutCycles(1ns, 3'700'000'000) == 4);
    CHECK(Common::MwaitxTimeoutCycles(1ns, 1'000'000'000) == 1);
    CHECK(Common::MwaitxTimeoutCycles(4'294'967'295ns, 1'000'000'000) == 0xffffffffU);
    CHECK(Common::MwaitxTimeoutCycles(4'294'967'296ns, 1'000'000'000) == 0xffffffffU);
}

TEST_CASE("Event resets cannot end an unsignalled timed wait", "[common][event]") {
    Common::Event event;
    std::jthread setter([&] {
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(100us);
            event.Reset();
        }
        event.Set();
    });
    CHECK(event.WaitFor(2s));
    setter.join();
    CHECK_FALSE(event.IsSet());
}

TEST_CASE("Event extreme timeouts preserve signal consumption", "[common][event]") {
    Common::Event event;
    CHECK_FALSE(event.WaitFor(std::chrono::nanoseconds::min()));
    event.Set();
    CHECK(event.WaitFor(std::chrono::nanoseconds::min()));
    CHECK_FALSE(event.IsSet());
    std::jthread setter([&] {
        std::this_thread::sleep_for(5ms);
        event.Set();
    });
    CHECK(event.WaitFor(std::chrono::nanoseconds::max()));
    setter.join();
    CHECK_FALSE(event.IsSet());
}

TEST_CASE("Event timed waits consume an existing signal", "[common][event]") {
    Common::Event event;
    CHECK_FALSE(event.WaitFor(0ns));
    CHECK_FALSE(event.WaitFor(-1ns));
    event.Set();
    CHECK(event.WaitFor(0ns));
    CHECK_FALSE(event.IsSet());
    CHECK_FALSE(event.WaitFor(0ns));
}

TEST_CASE("Event short timeouts do not return early", "[common][event]") {
    Common::Event event;
    for (const auto timeout : {200us, 500us, 2000us}) {
        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(event.WaitFor(timeout));
        CHECK(std::chrono::steady_clock::now() - start >= timeout);
    }
}

TEST_CASE("Event timeout expiry uses the steady clock", "[common][event]") {
    Common::Event event;
    for (const auto timeout : {100ms, 500ms}) {
        for (int trial = 0; trial < 2; ++trial) {
            const auto start = std::chrono::steady_clock::now();
            const bool signalled = event.WaitFor(timeout);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            CAPTURE(timeout, trial, elapsed);
            CHECK_FALSE(signalled);
            CHECK(elapsed >= timeout);
        }
    }
}

TEST_CASE("Event Set interrupts a long timed wait", "[common][event]") {
    Common::Event event;
    std::jthread setter([&] {
        std::this_thread::sleep_for(5ms);
        event.Set();
    });
    CHECK(event.WaitFor(2s));
    setter.join();
    CHECK_FALSE(event.WaitFor(0ns));
}

TEST_CASE("Event reset discards a prior timed-wait signal", "[common][event]") {
    Common::Event event;
    CHECK_FALSE(event.WaitFor(100us));
    event.Set();
    event.Reset();
    CHECK_FALSE(event.WaitFor(1ms));
    event.Set();
    CHECK(event.WaitFor(0ns));
}

TEST_CASE("Concurrent Event timed waits have independent deadlines", "[common][event]") {
    Common::Event event;
    std::barrier ready(3);
    bool short_result{}, long_result{};
    std::chrono::steady_clock::duration short_elapsed{}, long_elapsed{};
    std::jthread short_wait([&] {
        ready.arrive_and_wait();
        const auto start = std::chrono::steady_clock::now();
        short_result = event.WaitFor(20ms);
        short_elapsed = std::chrono::steady_clock::now() - start;
    });
    std::jthread long_wait([&] {
        ready.arrive_and_wait();
        const auto start = std::chrono::steady_clock::now();
        long_result = event.WaitFor(500ms);
        long_elapsed = std::chrono::steady_clock::now() - start;
    });
    ready.arrive_and_wait();
    short_wait.join();
    long_wait.join();
    CHECK_FALSE(short_result);
    CHECK_FALSE(long_result);
    CHECK(short_elapsed >= 20ms);
    CHECK(short_elapsed < 400ms);
    CHECK(long_elapsed >= 500ms);
}

TEST_CASE("Event signalling across timed and untimed waits remains consumable", "[common][event]") {
    Common::Event event;
    Common::Event acknowledgement;
    std::atomic<bool> completed{true};
    std::jthread worker([&] {
        for (int i = 0; i < 100; ++i) {
            if (!event.WaitFor(2s)) {
                completed = false;
                acknowledgement.Set();
                return;
            }
            acknowledgement.Set();
        }
    });
    for (int i = 0; i < 100; ++i) {
        event.Set();
        acknowledgement.Wait();
        if (!completed) {
            break;
        }
    }
    worker.join();
    CHECK(completed.load());
    CHECK_FALSE(event.WaitFor(0ns));
}
