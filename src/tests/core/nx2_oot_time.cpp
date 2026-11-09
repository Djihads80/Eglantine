// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_oot_time.h"

namespace {
using namespace std::chrono_literals;
using FrameDelta = Service::Nx2OotTime::FrameDelta;

float Run(FrameDelta& delta, FrameDelta::Clock::time_point& now, std::chrono::nanoseconds frame,
          int frames) {
    float result = 0.0f;
    for (int i = 0; i < frames; ++i) {
        now += frame;
        result = delta.Update(now);
    }
    return result;
}
} // namespace

TEST_CASE("NX2 Ocarina delta follows the measured frame time", "[core][nx2_oot_time]") {
    FrameDelta delta;
    FrameDelta::Clock::time_point now{1s};

    SECTION("The first present has no frame time yet") {
        REQUIRE(delta.Update(now) == 1.0f);
    }

    SECTION("60 fps keeps the game's fixed step") {
        REQUIRE(Run(delta, now, 16'666'666ns, 120) == 1.0f);
    }

    SECTION("Small jitter around 60 fps keeps the game's fixed step") {
        REQUIRE(Run(delta, now, 18ms, 120) == 1.0f);
    }

    SECTION("30 fps is a delta of 2") {
        REQUIRE(Run(delta, now, 33'333'333ns, 120) == Catch::Approx(2.0f).margin(0.01f));
    }

    SECTION("20 fps is a delta of 3") {
        REQUIRE(Run(delta, now, 50ms, 120) == Catch::Approx(3.0f).margin(0.01f));
    }

    SECTION("Long frames stop at the limit") {
        REQUIRE(Run(delta, now, 500ms, 120) == 4.0f);
    }

    SECTION("One stall doesn't jump the game forward") {
        Run(delta, now, 16'666'666ns, 120);
        now += 10s;
        REQUIRE(delta.Update(now) == Catch::Approx(1.625f).margin(0.01f));
    }
}
