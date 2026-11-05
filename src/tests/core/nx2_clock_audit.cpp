// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "yuzu_cmd/nx2_clock_audit.h"

namespace {
using namespace Nx2ClockAudit;
struct Fixture {
    Audit audit;
    std::size_t host_reads{};
    std::size_t guest_reads{};
    std::vector<Report> reports;
    Config config{31'250'000, 0, 100, false, true, true, false, 16000};

    void Poll(S64 before, S64 after, U64 ticks, bool enabled = true, bool running = true,
              bool stable = true) {
        std::size_t read_index{};
        const std::array times{before, after};
        audit.Poll(enabled, running,
            [&] { ++host_reads; REQUIRE(read_index < times.size()); return times[read_index++]; },
            [&] { ++guest_reads; return GuestRead{ticks, config, stable}; },
            [&](const Report& report) { reports.push_back(report); });
    }
};

TEST_CASE("Clock audit off and inactive states perform no clock or guest reads", "[nx2_clock_audit]") {
    CHECK(Enabled("1"));
    for (auto value : {"", "0", "true", "10", "1 "}) CHECK_FALSE(Enabled(value));
    Fixture fixture;
    fixture.Poll(0, 1, 0, false, true);
    fixture.Poll(0, 1, 0, true, false);
    CHECK(fixture.host_reads == 0);
    CHECK(fixture.guest_reads == 0);
    CHECK(fixture.reports.empty());
    for (const bool powered : {false, true}) {
        for (const bool paused : {false, true}) {
            const bool running = CanReadGuestCounter(powered, paused, false);
            CHECK_FALSE(running);
            fixture.Poll(0, 1, 0, true, running);
        }
    }
    CHECK(CanReadGuestCounter(true, false, true));
    CHECK_FALSE(CanReadGuestCounter(false, false, true));
    CHECK_FALSE(CanReadGuestCounter(true, true, true));
    CHECK(fixture.host_reads == 0);
    CHECK(fixture.guest_reads == 0);
}

TEST_CASE("Clock audit samples at most once per second without catch-up bursts", "[nx2_clock_audit]") {
    Fixture fixture;
    fixture.Poll(0, 100, 100);
    REQUIRE(fixture.reports.size() == 1);
    CHECK(fixture.reports.back().status == Status::Baseline);
    fixture.Poll(999'999'999, 0, 0);
    CHECK(fixture.guest_reads == 1);
    fixture.Poll(1'000'000'000, 1'000'000'100, 31'250'100);
    REQUIRE(fixture.reports.size() == 2);
    const auto& result = fixture.reports.back();
    CHECK(result.status == Status::Valid);
    CHECK(result.delta_ticks == 31'250'000);
    CHECK(result.wall_min_ns == 999'999'900);
    CHECK(result.wall_max_ns == 1'000'000'100);
    CHECK(std::abs(result.ratio_mid - 1.0) < 1e-12);
    CHECK(result.ratio_low < 1.0);
    CHECK(result.ratio_high > 1.0);
    fixture.Poll(10'000'000'000, 10'000'000'100, 312'500'100);
    CHECK(fixture.reports.size() == 3);
    fixture.Poll(10'000'000'001, 0, 0);
    CHECK(fixture.reports.size() == 3);
}

TEST_CASE("Clock audit exposes scaled counters instead of normalizing to expected speed", "[nx2_clock_audit]") {
    Fixture fixture;
    fixture.config.cpu_clock = 1; // The effective Boost enum; ratio must remain observable.
    fixture.Poll(0, 0, 100);
    fixture.Poll(1'000'000'000, 1'000'000'000, 53'125'100);
    REQUIRE(fixture.reports.back().status == Status::Valid);
    CHECK(std::abs(fixture.reports.back().ratio_mid - 1.7) < 1e-12);
    CHECK(fixture.reports.back().current.guest.config.cpu_clock == 1);
}

TEST_CASE("Clock audit rebaselines on config changes and pause resume", "[nx2_clock_audit]") {
    Fixture fixture;
    fixture.Poll(0, 1, 0);
    fixture.config.frequency = 19'200'000;
    fixture.Poll(1'000'000'000, 1'000'000'001, 19'200'000);
    CHECK(fixture.reports.back().status == Status::ConfigChanged);
    fixture.Poll(2'000'000'000, 2'000'000'001, 38'400'000);
    CHECK(fixture.reports.back().status == Status::Valid);
    fixture.config.sync_core_speed = true;
    fixture.config.speed_limit = 200;
    fixture.Poll(3'000'000'000, 3'000'000'001, 48'000'000);
    CHECK(fixture.reports.back().status == Status::ConfigChanged);
    const auto count = fixture.host_reads;
    fixture.Poll(3'100'000'000, 0, 0, true, false);
    CHECK(fixture.host_reads == count);
    fixture.Poll(3'200'000'000, 3'200'000'001, 49'920'000);
    CHECK(fixture.reports.back().status == Status::Baseline);
}

TEST_CASE("Clock audit rejects bad brackets unstable settings and regressed counters", "[nx2_clock_audit]") {
    Fixture fixture;
    fixture.Poll(0, 1, 100);
    fixture.Poll(1'000'000'000, 1'000'000'001, 99);
    CHECK(fixture.reports.back().status == Status::CounterRegressed);
    fixture.Poll(2'000'000'000, 1'999'999'999, 200);
    CHECK(fixture.reports.back().status == Status::InvalidSample);
    fixture.Poll(2'500'000'000, 0, 0); // Invalid samples still cannot cause a240Hz log flood.
    CHECK(fixture.reports.size() == 3);
    fixture.Poll(3'000'000'000, 3'000'000'001, 300, true, true, false);
    CHECK(fixture.reports.back().status == Status::InvalidSample);
    fixture.config.frequency = 0;
    fixture.Poll(4'000'000'000, 4'000'000'001, 400);
    CHECK(fixture.reports.back().status == Status::InvalidSample);
    fixture.config.frequency = 31'250'000;
    fixture.config.sync_core_speed = true;
    fixture.config.speed_limit = 0;
    fixture.Poll(5'000'000'000, 5'000'000'001, 500);
    CHECK(fixture.reports.back().status == Status::InvalidSample);
}

TEST_CASE("Clock audit handles overlapping brackets and large absolute counters", "[nx2_clock_audit]") {
    Fixture overlap;
    overlap.Poll(0, 2'000'000'000, 0);
    overlap.Poll(1'000'000'000, 3'000'000'000, 31'250'000);
    CHECK(overlap.reports.back().status == Status::Overlap);
    Fixture large;
    const S64 base = std::numeric_limits<S64>::max() - 2'000'000'000;
    const U64 tick_base = std::numeric_limits<U64>::max() - 31'250'000;
    large.Poll(base, base + 1, tick_base);
    large.Poll(base + 1'000'000'000, base + 1'000'000'001, std::numeric_limits<U64>::max());
    REQUIRE(large.reports.back().status == Status::Valid);
    CHECK(std::abs(large.reports.back().ratio_mid - 1.0) < 1e-12);
    CHECK(Difference(std::numeric_limits<S64>::max(), std::numeric_limits<S64>::min()) ==
          std::numeric_limits<U64>::max());
}
} // namespace
