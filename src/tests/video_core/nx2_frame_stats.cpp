// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_frame_stats.h"

namespace {

using namespace VideoCommon::Nx2FrameStats;
using Clock = std::chrono::steady_clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;

} // namespace

TEST_CASE("Nx2FrameStats nearest-rank percentiles", "[nx2_frame]") {
    std::vector<u32> values;
    for (u32 i = 100; i >= 1; --i) {
        values.push_back(i);
    }
    const Percentiles p = Compute(values);
    REQUIRE(p.p50 == 50);
    REQUIRE(p.p90 == 90);
    REQUIRE(p.p99 == 99);
    REQUIRE(p.max == 100);

    std::vector<u32> one{7};
    const Percentiles single = Compute(one);
    REQUIRE(single.p50 == 7);
    REQUIRE(single.p99 == 7);
    REQUIRE(single.max == 7);

    REQUIRE(Compute(std::span<u32>{}).max == 0);
}

TEST_CASE("Nx2FrameStats ring keeps an exact max past its size", "[nx2_frame]") {
    Ring<4> ring;
    std::array<u32, 4> scratch{};
    for (u32 value : {1000u, 1u, 2u, 3u, 4u, 5u}) {
        ring.Push(value);
    }
    REQUIRE(ring.Count() == 6);
    const Percentiles p = ring.Take(scratch);
    REQUIRE(p.max == 1000);
    REQUIRE(p.p50 == 3); // of the last four: 2 3 4 5
    REQUIRE(ring.Count() == 0);
    ring.Push(9);
    REQUIRE(ring.Take(scratch).max == 9);
}

TEST_CASE("Nx2FrameStats window closes once per period", "[nx2_frame]") {
    Window<64> window;
    const Clock::time_point start{milliseconds{1000}};
    Clock::time_point now = start;
    int closed = 0;
    // 60 presents 16 ms apart, then one 50 ms late; fence waits 0..60 us.
    for (u32 i = 0; i <= 62; ++i) {
        if (window.Add(now, microseconds{i})) {
            ++closed;
            const auto& last = window.Last();
            REQUIRE(last.presents == 63);
            REQUIRE(last.interval_us.p50 == 16'000);
            REQUIRE(last.interval_us.max == 50'000);
            REQUIRE(last.fence_us.max == 62);
            REQUIRE(last.fence_us.p50 == 31);
        }
        now += i == 61 ? milliseconds{50} : milliseconds{16};
    }
    // 61 * 16 ms + 50 ms = 1026 ms: the 63rd present closes the first window.
    REQUIRE(closed == 1);

    // The next window counts the interval from the present that closed the last one.
    REQUIRE_FALSE(window.Add(now, microseconds{5}));
    REQUIRE(window.Add(now + milliseconds{1000}, microseconds{7}));
    REQUIRE(window.Last().presents == 2);
    REQUIRE(window.Last().interval_us.max == 1'000'000);
    REQUIRE(window.Last().fence_us.max == 7);
}

TEST_CASE("Nx2FrameStats window clamps negative durations", "[nx2_frame]") {
    Window<8> window;
    const Clock::time_point start{milliseconds{1000}};
    REQUIRE_FALSE(window.Add(start, -microseconds{5}));
    REQUIRE(window.Add(start + milliseconds{1000}, microseconds{1}));
    REQUIRE(window.Last().fence_us.p50 == 0);
    REQUIRE(window.Last().fence_us.max == 1);
}
