// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include "common/common_types.h"

namespace VideoCommon {

[[nodiscard]] inline bool NativeTextureCacheBudgetEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_TC_BUDGET");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// EDEN_NX2_TC_BUDGET=1: the critical threshold on cards with more than 8 GiB of budget. The
/// budget-derived `critical_budget` already keeps the driver margins (1/8 of the budget, at most
/// 1 GiB, plus block slack and out-of-memory room), so it is used as is there. Ocarina of Time at
/// native 1x keeps ~12 GiB of render targets and textures in use while the camera moves (its
/// targets move to new addresses every few frames until the address ring wraps): a quarter of a
/// 14.5 GiB budget held back (10.9 GiB) left the collector evicting live images for good, 8-9k
/// images re-created per 600 frames; with the room it settles at ~30 per 600 and a steady 60 FPS.
/// Smaller cards keep `legacy_limit`.
[[nodiscard]] constexpr u64 NativeTextureCacheCriticalLimit(u64 critical_budget, u64 heap_budget,
                                                           u64 legacy_limit) {
    constexpr u64 small_card_budget = u64{8} << 30;
    const u64 limit = heap_budget <= small_card_budget ? legacy_limit
                                                       : (std::max)(legacy_limit, critical_budget);
    return (std::min)(critical_budget, limit);
}

[[nodiscard]] constexpr u64 TextureCacheCollectionAge(bool high_priority, bool aggressive,
                                                      u64 idle_age) {
    return aggressive ? 10 : high_priority ? 25 : (std::clamp<u64>)(idle_age, 50, 3600);
}

[[nodiscard]] inline u64 NativeTextureCacheIdleAge() {
    static const u64 age = [] {
        const char* value = std::getenv("EDEN_NX2_TC_IDLE_AGE");
        return value != nullptr ? static_cast<u64>(std::strtoull(value, nullptr, 10)) : u64{50};
    }();
    return age;
}

} // namespace VideoCommon
