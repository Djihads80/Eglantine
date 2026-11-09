// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_timestamp.h"

namespace {

struct Timing {
    std::chrono::nanoseconds time;
    u64 gpu_ticks;

    std::chrono::nanoseconds GetGlobalTimeNs() const {
        return time;
    }

    u64 GetGPUTicks() const {
        return gpu_ticks;
    }
};

constexpr std::array Clocks{Settings::GpuClock::Normal, Settings::GpuClock::Boost,
                            Settings::GpuClock::Overclock};

Timing At(u64 ns) {
    return {std::chrono::nanoseconds{ns}, ns * 384 / 625};
}

} // namespace

TEST_CASE("Ampere report timestamps use driver and VI nanoseconds", "[video_core][gpu_timestamp]") {
    constexpr u64 HostTimeNs = 293'036'000'000'000;
    for (const auto clock : Clocks) {
        CAPTURE(clock);
        REQUIRE(Tegra::ReadGpuTimestamp(At(HostTimeNs), true, clock, HostTimeNs) == HostTimeNs);
        REQUIRE(Tegra::ReadGpuTimestamp(At(HostTimeNs), true, clock, HostTimeNs + 1) == HostTimeNs);
    }
    REQUIRE(Tegra::ReadGpuTimestamp(At(HostTimeNs), true, Settings::GpuClock::Normal, 0) ==
            HostTimeNs);
}

TEST_CASE("Ampere CPU GPU calibration stays valid between samples", "[video_core][gpu_timestamp]") {
    constexpr u64 HostStartNs = 293'036'000'000'000;
    constexpr u64 GuestStartNs = 95'000'000'000;
    constexpr u64 Offset = GuestStartNs - HostStartNs;
    constexpr u64 GpuDurationNs = 5'000'000;
    constexpr u64 PresentDelayNs = 200'000;
    constexpr u64 SincePresentNs = 3'000'000;
    for (const auto clock : Clocks) {
        const u64 divisor = Tegra::AmpereClockDivisor(clock);
        for (u64 elapsed = 0; elapsed <= 1'000'000'000; elapsed += 31'250'000) {
            CAPTURE(clock, elapsed);
            const u64 start_ns = HostStartNs + elapsed;
            const u64 end_ns = start_ns + GpuDurationNs;
            const u64 present_ns = start_ns - SincePresentNs;
            const u64 gpu_begin = Tegra::ReadGpuTimestamp(At(start_ns), true, clock, present_ns);
            const u64 gpu_end = Tegra::ReadGpuTimestamp(At(end_ns), true, clock, present_ns);
            const u64 present = end_ns + PresentDelayNs + Offset;
            REQUIRE(gpu_begin + Offset == GuestStartNs + elapsed - SincePresentNs +
                                              SincePresentNs / divisor);
            REQUIRE(gpu_end - gpu_begin == GpuDurationNs / divisor);
            REQUIRE(present - (gpu_end + Offset) ==
                    PresentDelayNs + (GpuDurationNs + SincePresentNs) -
                        (GpuDurationNs + SincePresentNs) / divisor);
        }
    }
}

TEST_CASE("Ampere boosted timestamps stay monotonic across presents and within a frame of CPU time",
          "[video_core][gpu_timestamp]") {
    constexpr u64 StartNs = 293'036'000'000'000;
    constexpr u64 FrameNs = 16'666'667;
    for (const auto clock : Clocks) {
        u64 present_ns = StartNs;
        u64 previous = 0;
        for (u64 now = StartNs; now <= StartNs + 10 * FrameNs; now += 1'000'003) {
            if (now >= present_ns + FrameNs) {
                present_ns += FrameNs;
            }
            CAPTURE(clock, now, present_ns);
            const u64 stamp = Tegra::ReadGpuTimestamp(At(now), true, clock, present_ns);
            REQUIRE(stamp >= previous);
            REQUIRE(stamp <= now);
            REQUIRE(now - stamp < FrameNs);
            previous = stamp;
        }
    }
}

TEST_CASE("Maxwell retains its tick units and clock scaling", "[video_core][gpu_timestamp]") {
    for (const u64 ticks : {u64{0}, u64{1}, u64{255}, u64{256}, u64{511},
                           u64{512}, u64{614'400'000}, u64{180'041'318'400'000}}) {
        CAPTURE(ticks);
        const Timing timing{std::chrono::nanoseconds{293'036'000'000'000}, ticks};
        for (const u64 present_ns : {u64{0}, u64{293'035'990'000'000}}) {
            REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Normal,
                                            present_ns) == ticks);
            REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Boost,
                                            present_ns) == ticks / 256);
            REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Overclock,
                                            present_ns) == ticks / 512);
        }
    }
}
