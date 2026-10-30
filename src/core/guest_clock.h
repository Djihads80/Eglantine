// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hardware_properties.h"

namespace Core::Timing {

// Switch 2 applications read CNTFRQ_EL0 as 31.25 MHz. Told 19.2 MHz while the counter advanced
// at that rate, title 4 paced its frames to 60 * 19.2 / 31.25 = 37 fps.
constexpr u32 GetGuestCounterFrequency(bool switch2) {
    return switch2 ? 31'250'000U : static_cast<u32>(Hardware::CNTFREQ);
}

// Scheduler time stays in CNTFREQ units; only the counter the guest sees is converted.
constexpr u64 GetGuestCounterTicks(u64 ticks, bool switch2) {
    if (!switch2) {
        return ticks;
    }
    const u64 frequency = GetGuestCounterFrequency(true);
    return (ticks / Hardware::CNTFREQ) * frequency +
           (ticks % Hardware::CNTFREQ) * frequency / Hardware::CNTFREQ;
}

static_assert(GetGuestCounterTicks(19'200'000, true) == 31'250'000);
static_assert(GetGuestCounterTicks(9'600'000, true) == 15'625'000);
static_assert(GetGuestCounterTicks(12345, false) == 12345);

} // namespace Core::Timing
