// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include "common/common_types.h"
#include "common/literals.h"

namespace VideoCommon {
struct TextureCacheMemoryThresholds {
    u64 expected, critical, minimum;
};

// Budget-driven callers have already reserved driver/allocator pressure headroom.
// Retain the legacy ceiling when there is no reliable, scoped device budget.
constexpr TextureCacheMemoryThresholds ClampTextureCacheThresholds(
    TextureCacheMemoryThresholds limits, bool budget_driven, u64 legacy_ceiling) {
    using namespace Common::Literals;
    if (!budget_driven)
        limits.critical = (std::min)(limits.critical, legacy_ceiling);
    limits.expected =
        (std::min)(limits.expected, limits.critical - (std::min)(limits.critical, u64{768_MiB}));
    limits.minimum =
        (std::min)(limits.minimum, limits.expected - (std::min)(limits.expected, u64{1_GiB}));
    return limits;
}
} // namespace VideoCommon
