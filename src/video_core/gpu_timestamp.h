// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

/// How much faster Boost and Overclock make Switch 2 GPU work look.
[[nodiscard]] constexpr u64 AmpereClockDivisor(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 4;
    case Settings::GpuClock::Overclock:
        return 16;
    default:
        return 1;
    }
}

/// Switch 2 titles that keep the pre-nanosecond timestamps (GPU ticks divided by the clock setting,
/// the non-ampere path below). Mario Kart World sizes its dynamic resolution from GPU timestamps:
/// with nanoseconds the emulator's GPU latency reads as a nearly full frame budget, the resolution
/// changes every few frames, and each change recreates and re-uploads its render targets (~20 fps
/// instead of ~28-30 on an RX 6700S, 16x the texture cache image creations).
[[nodiscard]] constexpr bool UsesLegacyAmpereTimestamps(u64 program_id) {
    constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
    return program_id == MarioKartWorldProgramId;
}

/// Switch 2 titles read report timestamps as nanoseconds and calibrate them against CPU time, so
/// Boost and Overclock shrink only the time since the last present (present_ns): GPU durations
/// within a frame look shorter, while a timestamp never runs ahead of the CPU clock and trails it
/// by less than the time since that present.
template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock,
                                   u64 present_ns) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        if (now <= present_ns) {
            return now;
        }
        return present_ns + (now - present_ns) / AmpereClockDivisor(clock);
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
