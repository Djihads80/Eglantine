// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <unordered_map>

#include "common/common_types.h"

namespace Tegra {

// The puller's semaphore releases as the split GPU thread's front queued them (gpu_split.h),
// by device address: a channel's acquire can be met without draining the back when the last
// release queued for its word already meets it (the back executes the ring in order, so
// everything after the acquire runs after that release) or when guest memory already holds a
// value that meets it and no queued puller release would move the word away from it.
// Releases the front does not see (QMD and engine semaphores, DMA copies, CPU writes) are not
// recorded: an acquire on such a word still drains the back and reads it unless guest memory
// already meets it. That last case assumes the word is not moved backwards by GPU work queued
// before the acquire (NVN sync objects count up). EDEN_NX2_ACQ_LEDGER=0 turns the ledger off.
class Nx2SemaphoreLedger {
public:
    enum class Compare : u32 {
        Equal,          ///< word == value
        Gequal,         ///< word >= value (unsigned)
        CircularGequal, ///< (s32)(word - value) >= 0
        AndNonZero,     ///< (word & value) != 0
        Nor,            ///< (word & value) == 0
    };

    struct Entry {
        u64 order; ///< Ring position after the release was sent (GpuSplit::WritePosition)
        u32 value;
    };

    static Nx2SemaphoreLedger& Get() {
        static Nx2SemaphoreLedger ledger;
        return ledger;
    }

    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_ACQ_LEDGER");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    /// A 32-bit puller release of `value` to the word at `device_address` was queued.
    void NoteRelease(u64 device_address, u32 value, u64 order) {
        entries[device_address] = Entry{order, value};
    }

    /// The word gets a value the front cannot know (reduction, 64-bit payload).
    void Forget(u64 device_address) {
        entries.erase(device_address);
    }

    [[nodiscard]] const Entry* Find(u64 device_address) const {
        const auto it = entries.find(device_address);
        return it == entries.end() ? nullptr : &it->second;
    }

    [[nodiscard]] static bool Met(Compare compare, u32 word, u32 value) {
        switch (compare) {
        case Compare::Equal:
            return word == value;
        case Compare::Gequal:
            return word >= value;
        case Compare::CircularGequal:
            return static_cast<s32>(word - value) >= 0;
        case Compare::AndNonZero:
            return (word & value) != 0;
        case Compare::Nor:
            return (word & value) == 0;
        }
        return false;
    }

private:
    std::unordered_map<u64, Entry> entries;
};

} // namespace Tegra
