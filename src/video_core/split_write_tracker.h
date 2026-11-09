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
        /// Only draws whose exact programs the back proved write no guest memory
        /// (Nx2Perf MacroReadWriterSet, Tegra::Nx2IsNonWriterDraw)
        Draw,
    };

    /// What a noted write stores: constant buffer data the front queued (its words are known to
    /// the front) or anything else.
    enum class WriteKind : u8 {
        Other,
        ConstantBuffer,
    };

    /// The effect of an entry that does both.
    [[nodiscard]] static constexpr Effect Combine(Effect a, Effect b) {
        if (a == Effect::Unknown || b == Effect::Unknown) {
            return Effect::Unknown;
        }
        if (a == Effect::Gpu || b == Effect::Gpu) {
            return Effect::Gpu;
        }
        return a == Effect::Draw || b == Effect::Draw ? Effect::Draw : Effect::None;
    }

    /// More pending ranges than this collapse into one unknown write.
    static constexpr size_t MaxWrites = 512;

    void Entry(Effect effect, u64 end) {
        if (effect == Effect::Unknown) {
            unknown = end;
        } else if (effect == Effect::Gpu) {
            gpu = end;
            writer = end;
        } else if (effect == Effect::Draw) {
            gpu = end;
            if (draw_epochs.empty() || draw_epochs.back().epoch != entry_epoch) {
                draw_epochs.push_back({end, entry_epoch});
            } else {
                draw_epochs.back().position = end;
            }
        }
        entry_epoch = 0;
    }

    /// The next entry's draws were classified with the program epoch `epoch`
    /// (Tegra::g_nx2_code_epoch). Without it a Draw entry never validates (DrawsValid).
    void DrawEpoch(u64 epoch) {
        entry_epoch = entry_epoch == 0 ? epoch : (std::min)(entry_epoch, epoch);
    }

    /// The entry that ends at `end` writes the guest memory in [begin, end_address). Ends never
    /// decrease. A write next to or over the previous one joins it (conservative: the joined
    /// range retires with the later write; one that mixes kinds is Other).
    void Write(u64 begin, u64 end_address, u64 end, WriteKind kind = WriteKind::Other) {
        if (begin >= end_address) {
            return;
        }
        if (!writes.empty()) {
            PendingWrite& last = writes.back();
            if (begin <= last.end && last.begin <= end_address) {
                last.begin = (std::min)(last.begin, begin);
                last.end = (std::max)(last.end, end_address);
                last.position = end;
                if (last.kind != kind) {
                    last.kind = WriteKind::Other;
                }
                return;
            }
        }
        if (writes.size() >= MaxWrites) {
            unknown = (std::max)(unknown, end);
            writes.clear();
            return;
        }
        writes.push_back({begin, end_address, end, kind});
    }

    /// Drops the writes the back has run (it consumed `consumed` words).
    void Retire(u64 consumed) {
        while (!writes.empty() && writes.front().position <= consumed) {
            writes.pop_front();
        }
        while (!draw_epochs.empty() && draw_epochs.front().position <= consumed) {
            draw_epochs.pop_front();
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

    /// LastWriteTo for the noted writes of `kind` only.
    [[nodiscard]] u64 LastWriteTo(u64 begin, u64 end, WriteKind kind) const {
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->kind == kind && it->begin < end && begin < it->end) {
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

    /// ReadTarget without the draws the back proved write no memory (Effect::Draw): every
    /// unknown write, every writer (Effect::Gpu) and the noted writes to [begin, end). Valid only
    /// while DrawsValid holds for the draws it skips.
    [[nodiscard]] u64 WriterReadTarget(u64 begin, u64 end) const {
        return (std::max)({unknown, writer, LastWriteTo(begin, end)});
    }

    /// Whether every Effect::Draw entry past `consumed` was classified with `epoch`, so the
    /// program proofs they relied on still hold.
    [[nodiscard]] bool DrawsValid(u64 consumed, u64 epoch) const {
        for (const EpochRecord& record : draw_epochs) {
            if (record.position > consumed && record.epoch != epoch) {
                return false;
            }
        }
        return true;
    }

    /// Whether a noted write past `consumed` and up to `limit` has a range `touches(begin, end)`.
    template <typename Pred>
    [[nodiscard]] bool AnyPendingWrite(u64 consumed, u64 limit, Pred&& touches) const {
        for (const PendingWrite& write : writes) {
            if (write.position > limit) {
                break;
            }
            if (write.position > consumed && touches(write.begin, write.end)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] u64 LastUnknown() const {
        return unknown;
    }
    [[nodiscard]] u64 LastGpu() const {
        return gpu;
    }
    void Reset() {
        writes.clear();
        draw_epochs.clear();
        unknown = 0;
        gpu = 0;
        writer = 0;
        entry_epoch = 0;
    }

private:
    struct PendingWrite {
        u64 begin;
        u64 end;
        u64 position;
        WriteKind kind;
    };
    struct EpochRecord {
        u64 position; ///< Just past the newest Draw entry classified with `epoch`
        u64 epoch;
    };
    std::deque<PendingWrite> writes;
    std::deque<EpochRecord> draw_epochs;
    u64 unknown{};
    u64 gpu{};    ///< Past the newest entry that draws, dispatches or copies (Gpu or Draw)
    u64 writer{}; ///< Past the newest Gpu entry: the ones that can write guest memory
    u64 entry_epoch{};
};

} // namespace VideoCommon
