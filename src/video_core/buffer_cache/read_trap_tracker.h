// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace VideoCommon {

class ReadTrapTracker {
public:
    /// Arms this page-aligned range. Calls `arm(address, size)` for each part that was not armed
    /// yet, the parts the core must be notified of, and returns whether there was any.
    template <typename Func>
    bool Arm(DAddr address, u64 size, Func&& arm) {
        const DAddr end = address + size;
        DAddr next = address;
        bool any = false;
        ranges.ForEachInRange(address, size, [&](DAddr lower, DAddr upper) {
            if (lower > next) {
                arm(next, lower - next);
                any = true;
            }
            next = (std::max)(next, upper);
        });
        if (next < end) {
            arm(next, end - next);
            any = true;
        }
        if (any) {
            ranges.Add(address, size);
        }
        return any;
    }

    /// Returns whether the core must be notified to arm this page-aligned range.
    bool Arm(DAddr address, u64 size) {
        return Arm(address, size, [](DAddr, u64) {});
    }

    /// A CPU read consumes the core's trap; unmapping also forgets the range.
    void Subtract(DAddr address, u64 size) {
        ranges.Subtract(address, size);
    }

private:
    Common::RangeSet<DAddr> ranges;
};

} // namespace VideoCommon
