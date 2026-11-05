// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <deque>

#include "common/common_types.h"

namespace VideoCommon {

/// What the entries the split GPU thread's front queued may write to guest memory, so a read on
/// the front (MME DMA) waits only for the queued work it depends on. Positions are ring positions
/// just past an entry: the back has run the entry once it consumed that far.
class SplitWriteTracker {
public:
    enum class Effect : u8 {
        Unknown, ///< May write guest memory anywhere at once
        None,    ///< Writes no guest memory, or only the ranges noted with Write
        Gpu,     ///< Draws, dispatches and copies with destinations not known to the front
    };

    /// The effect of an entry that does both.
    [[nodiscard]] static constexpr Effect Combine(Effect a, Effect b) {
        if (a == Effect::Unknown || b == Effect::Unknown) {
            return Effect::Unknown;
        }
        return a == Effect::Gpu || b == Effect::Gpu ? Effect::Gpu : Effect::None;
    }

    /// More pending ranges than this collapse into one unknown write.
    static constexpr size_t MaxWrites = 512;

    void Entry(Effect effect, u64 end) {
        if (effect == Effect::Unknown) {
            unknown = end;
        } else if (effect == Effect::Gpu) {
            gpu = end;
        }
    }

    /// The entry that ends at `end` writes the guest memory in [begin, end_address). Ends never
    /// decrease. A write next to or over the previous one joins it (conservative: the joined
    /// range retires with the later write).
    void Write(u64 begin, u64 end_address, u64 end) {
        if (begin >= end_address) {
            return;
        }
        if (!writes.empty()) {
            PendingWrite& last = writes.back();
            if (begin <= last.end && last.begin <= end_address) {
                last.begin = (std::min)(last.begin, begin);
                last.end = (std::max)(last.end, end_address);
                last.position = end;
                return;
            }
        }
        if (writes.size() >= MaxWrites) {
            unknown = (std::max)(unknown, end);
            writes.clear();
            return;
        }
        writes.push_back({begin, end_address, end});
    }

    /// Drops the writes the back has run (it consumed `consumed` words).
    void Retire(u64 consumed) {
        while (!writes.empty() && writes.front().position <= consumed) {
            writes.pop_front();
        }
    }

    /// The position the back must pass for the noted writes to [begin, end) to be done, or 0.
    [[nodiscard]] u64 LastWriteTo(u64 begin, u64 end) const {
        // Positions grow towards the back: the newest overlap is the latest.
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->begin < end && begin < it->end) {
                return it->position;
            }
        }
        return 0;
    }

    /// The position the back must pass for every unlocalized writer and the noted writes to
    /// [begin, end) to execute. Consumption does not imply completion of deferred fence writes.
    [[nodiscard]] u64 ReadTarget(u64 begin, u64 end) const {
        return (std::max)({unknown, gpu, LastWriteTo(begin, end)});
    }

    [[nodiscard]] u64 LastUnknown() const {
        return unknown;
    }
    [[nodiscard]] u64 LastGpu() const {
        return gpu;
    }

    void Reset() {
        writes.clear();
        unknown = 0;
        gpu = 0;
    }

private:
    struct PendingWrite {
        u64 begin;
        u64 end;
        u64 position;
    };
    std::deque<PendingWrite> writes;
    u64 unknown{};
    u64 gpu{};
};

} // namespace VideoCommon
