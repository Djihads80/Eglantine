// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <limits>

#include "common/common_types.h"

namespace Common {

constexpr std::chrono::nanoseconds EventWaitDeadline(std::chrono::nanoseconds now,
                                                     std::chrono::nanoseconds timeout) {
    if (timeout <= std::chrono::nanoseconds::zero()) {
        return now;
    }
    return now > std::chrono::nanoseconds::max() - timeout
               ? std::chrono::nanoseconds::max()
               : now + timeout;
}

constexpr u32 MwaitxTimeoutCycles(std::chrono::nanoseconds remaining, u64 ticks_per_second) {
    if (remaining <= std::chrono::nanoseconds::zero()) {
        return 0;
    }
    constexpr u64 MaxCycles = std::numeric_limits<u32>::max();
    constexpr u64 NsPerSecond = 1'000'000'000;
    const u64 ns = static_cast<u64>(remaining.count());
    if (ns > MaxCycles * NsPerSecond / ticks_per_second) {
        return static_cast<u32>(MaxCycles);
    }
    return static_cast<u32>((ns * ticks_per_second + NsPerSecond - 1) / NsPerSecond);
}

} // namespace Common
