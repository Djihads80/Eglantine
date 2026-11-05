// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <limits>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon {
struct IndirectResetListRange {
    u64 begin;
    size_t size;
};

[[nodiscard]] inline std::optional<IndirectResetListRange> IndirectResetRange(
    u64 list, u32 list_size, u64 first, u64 count) {
    const u64 entries = list_size / sizeof(u64);
    constexpr u64 max_entries = 1U << 20;
    if (!list || !count || count > max_entries || first > entries || count > entries - first ||
        list > (std::numeric_limits<u64>::max)() - first * sizeof(u64)) {
        return std::nullopt;
    }
    const u64 begin = list + first * sizeof(u64);
    const size_t size = static_cast<size_t>(count * sizeof(u64));
    if (begin > (std::numeric_limits<u64>::max)() - size) {
        return std::nullopt;
    }
    return IndirectResetListRange{begin, size};
}
} // namespace VideoCommon
