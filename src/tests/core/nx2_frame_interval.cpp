// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_frame_interval.h"

namespace {
using namespace std::chrono_literals;
using Controller = Service::Nx2FrameInterval::Controller;
constexpr Controller::Duration Nominal{1000.0 / 60.0};

u32 Run(Controller& controller, Controller::Clock::time_point& now, std::chrono::nanoseconds frame,
        int frames) {
    u32 interval = controller.Interval();
    for (int i = 0; i < frames; ++i) {
        now += frame;
        interval = controller.Update(now, Nominal);
    }
    return interval;
}

/// Frames of `frame` until the interval becomes `want`; returns the time that took.
std::chrono::nanoseconds Until(Controller& controller, Controller::Clock::time_point& now,
                               std::chrono::nanoseconds frame, u32 want) {
    const auto start = now;
    for (int i = 0; i < 2000 && controller.Interval() != want; ++i) {
        Run(controller, now, frame, 1);
    }
    return now - start;
}
} // namespace

TEST_CASE("NX2 frame interval follows whether 60 fps holds", "[core][nx2_frame_interval]") {
    Controller controller;
    Controller::Clock::time_point now{1s};
    REQUIRE(controller.Update(now, Nominal) == 1);

    SECTION("60 fps stays at 1") {
        REQUIRE(Run(controller, now, 16'666'666ns, 600) == 1);
    }

    SECTION("Jitter around 60 fps stays at 1") {
        for (int i = 0; i < 300; ++i) {
            REQUIRE(Run(controller, now, i % 2 ? 15ms : 18ms, 1) == 1);
        }
    }

    SECTION("57 fps stays at 1") {
        REQUIRE(Run(controller, now, 17'400'000ns, 600) == 1);
    }

    SECTION("45 fps drops to 2 within 1.5 s") {
        REQUIRE(Run(controller, now, 22ms, 59) == 1);
        REQUIRE(Run(controller, now, 22ms, 1) == 2);
    }

    SECTION("A loading stall doesn't count") {
        REQUIRE(Run(controller, now, 16'666'666ns, 40) == 1);
        REQUIRE(Run(controller, now, 2s, 3) == 1);
        REQUIRE(Run(controller, now, 16'666'666ns, 40) == 1);
    }

    SECTION("A failed try at 60 goes back to 2 within a few frames, then waits longer") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        const auto first = Until(controller, now, 33'333'333ns, 1);
        REQUIRE(first >= 2s);
        REQUIRE(first < 2s + 34ms);
        REQUIRE(Run(controller, now, 35ms, 9) == 1); // 2 skipped + 7 measured
        REQUIRE(Run(controller, now, 35ms, 1) == 2); // 8 measured below 40 fps
        const auto second = Until(controller, now, 33'333'333ns, 1);
        REQUIRE(second >= 4s);
        REQUIRE(second < 4s + 34ms);
    }

    SECTION("The wait between failed tries stops growing at 16 s") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        std::chrono::nanoseconds wait{};
        for (int i = 0; i < 6; ++i) {
            wait = Until(controller, now, 33'333'333ns, 1);
            REQUIRE(Until(controller, now, 35ms, 2) < 400ms);
        }
        REQUIRE(wait >= 16s);
        REQUIRE(wait < 16s + 34ms);
    }

    SECTION("A successful try at 60 stays at 1") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        Until(controller, now, 33'333'333ns, 1);
        REQUIRE(Run(controller, now, 16'666'666ns, 600) == 1);
    }

    SECTION("A try at 57 fps stays at 1") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        Until(controller, now, 33'333'333ns, 1);
        REQUIRE(Run(controller, now, 17'400'000ns, 600) == 1);
    }

    SECTION("Spiky frames averaging 57 fps stay at 1, also after a try") {
        const auto spiky = [&](int frames) {
            u32 interval = 0;
            for (int i = 0; i < frames; ++i) {
                interval = Run(controller, now, i % 10 == 9 ? 36ms : 15ms, 1);
            }
            return interval;
        };
        REQUIRE(spiky(600) == 1);
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        Until(controller, now, 33'333'333ns, 1);
        REQUIRE(spiky(600) == 1);
    }

    SECTION("A pause during a try forgets the frames before it") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        Until(controller, now, 33'333'333ns, 1);
        REQUIRE(Run(controller, now, 35ms, 5) == 1); // 2 skipped + 3 slow measured
        REQUIRE(Run(controller, now, 2s, 1) == 1);
        REQUIRE(Run(controller, now, 16'666'666ns, 40) == 1);
    }

    SECTION("A try at 50 fps goes back to 2 after half a second") {
        REQUIRE(Run(controller, now, 22ms, 60) == 2);
        Until(controller, now, 33'333'333ns, 1);
        REQUIRE(Run(controller, now, 20ms, 31) == 1);
        REQUIRE(Run(controller, now, 20ms, 1) == 2);
    }
}
