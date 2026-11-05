// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace VideoCommon {

class ReadTrapTracker {
public:
    /// Returns whether the core must be notified to arm this page-aligned range.
    bool Arm(DAddr address, u64 size) {
        u64 armed_bytes = 0;
        ranges.ForEachInRange(address, size, [&](DAddr lower, DAddr upper) {
            armed_bytes += upper - lower;
        });
        if (armed_bytes == size) {
            return false;
        }
        ranges.Add(address, size);
        return true;
    }

    /// A CPU read consumes the core's trap; unmapping also forgets the range.
    void Subtract(DAddr address, u64 size) {
        ranges.Subtract(address, size);
    }

private:
    Common::RangeSet<DAddr> ranges;
};

} // namespace VideoCommon
