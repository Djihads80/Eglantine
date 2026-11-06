// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "common/literals.h"

namespace VideoCommon::TextureCacheMemory {

using namespace Common::Literals;

/// The lowest limit the texture cache accepts, however small a value is set.
inline constexpr u64 MinLimit = 1_GiB;

/// What Auto (a limit of 0) uses: three quarters of the device memory.
[[nodiscard]] constexpr u64 RecommendedLimit(u64 device_memory) {
    return device_memory / 4 * 3;
}

/// The limit in bytes for a setting value in MiB. Without a known device memory (0) there is
/// nothing to follow or clamp to.
[[nodiscard]] constexpr u64 ResolveLimit(u32 setting_mib, u64 device_memory) {
    if (device_memory == 0) {
        return setting_mib != 0 ? u64{setting_mib} << 20 : ~u64{0};
    }
    if (setting_mib == 0) {
        return RecommendedLimit(device_memory);
    }
    return std::clamp(u64{setting_mib} << 20, (std::min)(MinLimit, device_memory), device_memory);
}

/// True when an explicit limit sits under what Auto would use.
[[nodiscard]] constexpr bool IsBelowRecommended(u32 setting_mib, u64 device_memory) {
    return setting_mib != 0 && device_memory != 0 &&
           (u64{setting_mib} << 20) < RecommendedLimit(device_memory);
}

} // namespace VideoCommon::TextureCacheMemory
