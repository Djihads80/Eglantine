// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"

namespace VideoCommon {

/// The bytes of a command list page GPU work wrote since the front last flushed it, as offsets in
/// the page. They are known only from a flush on: writes before it were not noted. A list on none
/// of them reads guest memory (DK Bananza's texture binding lists share their pages with
/// GPU-written data and waited for its copies).
struct Nx2ListRanges {
    static constexpr size_t MaxRanges = 8; ///< Past this many, one bounding range

    boost::container::small_vector<std::pair<u32, u32>, 4> ranges;
    bool known{};

    /// The front flushed the page: guest memory holds all of it.
    void Reset() {
        ranges.clear();
        known = true;
    }

    /// GPU work wrote [begin, end) of the page.
    void Note(u32 begin, u32 end) {
        if (begin >= end) {
            return;
        }
        for (auto& range : ranges) {
            if (begin <= range.second && range.first <= end) {
                range = {(std::min)(range.first, begin), (std::max)(range.second, end)};
                return;
            }
        }
        if (ranges.size() < MaxRanges) {
            ranges.emplace_back(begin, end);
            return;
        }
        u32 first = begin;
        u32 last = end;
        for (const auto& range : ranges) {
            first = (std::min)(first, range.first);
            last = (std::max)(last, range.second);
        }
        ranges.assign(1, {first, last});
    }

    /// Guest memory got [begin, end) (a CPU write, an inline upload): its bytes are current
    /// there until GPU work writes them again. Left as it is when splitting a range would need
    /// more than MaxRanges.
    void Clear(u32 begin, u32 end) {
        if (!known || begin >= end) {
            return;
        }
        boost::container::small_vector<std::pair<u32, u32>, 4> result;
        for (const auto& range : ranges) {
            if (end <= range.first || range.second <= begin) {
                result.push_back(range);
                continue;
            }
            if (range.first < begin) {
                result.emplace_back(range.first, begin);
            }
            if (end < range.second) {
                result.emplace_back(end, range.second);
            }
        }
        if (result.size() <= MaxRanges) {
            ranges = std::move(result);
        }
    }

    /// Whether GPU work may have written any of [begin, end) since the last flush.
    [[nodiscard]] bool Wrote(u64 begin, u64 end) const {
        if (!known) {
            return true;
        }
        return std::ranges::any_of(ranges, [begin, end](const auto& range) {
            return begin < range.second && range.first < end;
        });
    }
};

} // namespace VideoCommon
