// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"
#include "common/logging.h"

namespace Core {
extern std::atomic<u64> g_nx2_present_count; // core.h
}

namespace VideoCommon::Nx2Hazard {

// EDEN_NX2_HAZARD=1 (log only): finds guest memory the GPU back thread reads for one frame after
// the guest already rewrote it for a later one (what a present that doesn't wait for its own
// frame allows), and GPU-written bytes the buffer cache drops because a guest write touched the
// same 64-byte unit.
// Epoch = the number of presents the guest had made. A guest write is stamped with the epoch it
// happened in; each submitted command list carries the epoch it was submitted in and the back
// knows the epoch of the list it runs. A read of a range stamped with a later epoch is a hazard.

enum class Site : u32 {
    UniformFast,
    UniformCompute,
    UniformReuse,
    ImmediateUpload,
    MappedUpload,
    ClaimGpuModified,
    WriteGpuModified,
    TextureUpload,
    Count,
};

inline constexpr const char* SiteNames[] = {
    "uniform (fast bind)", "uniform (compute)", "uniform (stream reuse)",
    "buffer immediate upload", "buffer mapped upload", "claim of GPU-modified range",
    "write over GPU-modified range", "texture upload"};

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_HAZARD");
        return value != nullptr && (value[0] == '1' || value[0] == '2');
    }();
    return enabled;
}

/// EDEN_NX2_HAZARD=2 logs every hazard, not the first few.
[[nodiscard]] inline bool Verbose() {
    static const bool verbose = [] {
        const char* value = std::getenv("EDEN_NX2_HAZARD");
        return value != nullptr && value[0] == '2';
    }();
    return verbose;
}

inline std::atomic<u64> g_back_epoch{0};

[[nodiscard]] inline u64 CurrentEpoch() {
    return Core::g_nx2_present_count.load(std::memory_order_relaxed);
}

/// The back (or the single GPU thread) is about to run a command list submitted in `epoch`.
inline void SetListEpoch(u64 epoch) {
    g_back_epoch.store(epoch, std::memory_order_relaxed);
}

namespace Detail {
constexpr u64 TableBits = 22;
constexpr u64 EpochBits = 24;
constexpr u64 EpochMask = (u64{1} << EpochBits) - 1;
constexpr u64 MaxUnits = 1024; ///< 64 KiB checked per read

inline std::atomic<u64>* Table() {
    static std::atomic<u64>* const table = static_cast<std::atomic<u64>*>(
        std::calloc(size_t{1} << TableBits, sizeof(std::atomic<u64>)));
    return table;
}

struct Counters {
    std::atomic<u64> checks[static_cast<size_t>(Site::Count)]{};
    std::atomic<u64> hazards[static_cast<size_t>(Site::Count)]{};
    std::atomic<u64> bytes[static_cast<size_t>(Site::Count)]{};
    std::atomic<u64> read_bytes[static_cast<size_t>(Site::Count)]{};
};
inline Counters g_counters;

/// Newest write epoch stamped on a unit of [addr, addr + size), 0 when none; fills `newest_unit`.
inline u64 NewestWrite(DAddr addr, u64 size, u64& newest_unit) {
    std::atomic<u64>* const table = Table();
    const u64 first = addr >> 6;
    const u64 last = (addr + (size == 0 ? 1 : size) - 1) >> 6;
    u64 newest = 0;
    for (u64 unit = first; unit <= last && unit - first < MaxUnits; ++unit) {
        const u64 entry = table[unit & ((u64{1} << TableBits) - 1)].load(std::memory_order_relaxed);
        if ((entry >> EpochBits) == unit && (entry & EpochMask) > newest) {
            newest = entry & EpochMask;
            newest_unit = unit;
        }
    }
    return newest;
}
} // namespace Detail

/// A guest write to memory the rasterizer caches (any thread).
inline void NoteGuestWrite(DAddr addr, u64 size) {
    if (!Enabled()) {
        return;
    }
    std::atomic<u64>* const table = Detail::Table();
    const u64 epoch = CurrentEpoch() & Detail::EpochMask;
    const u64 first = addr >> 6;
    const u64 last = (addr + (size == 0 ? 1 : size) - 1) >> 6;
    for (u64 unit = first; unit <= last && unit - first < 4096; ++unit) {
        table[unit & ((u64{1} << Detail::TableBits) - 1)].store((unit << Detail::EpochBits) | epoch,
                                                              std::memory_order_relaxed);
    }
}

/// The back reads guest memory [addr, addr + size) for the list it runs.
inline void CheckRead(Site site, DAddr addr, u64 size) {
    if (!Enabled()) {
        return;
    }
    const size_t index = static_cast<size_t>(site);
    Detail::g_counters.checks[index].fetch_add(1, std::memory_order_relaxed);
    Detail::g_counters.read_bytes[index].fetch_add(size, std::memory_order_relaxed);
    u64 unit = 0;
    const u64 written = Detail::NewestWrite(addr, size, unit);
    const u64 list = g_back_epoch.load(std::memory_order_relaxed) & Detail::EpochMask;
    if (written <= list) {
        return;
    }
    Detail::g_counters.bytes[index].fetch_add(size, std::memory_order_relaxed);
    const u64 count = Detail::g_counters.hazards[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 24 || count % 2000 == 0 || Verbose()) {
        LOG_INFO(HW_GPU,
                 "nx2 hazard: {} reads [{:#x}, +{:#x}) for a list of epoch {}, but the guest wrote "
                 "{:#x} in epoch {} (#{}, {} checks)",
                 SiteNames[index], addr, size, list, unit << 6, written, count,
                 Detail::g_counters.checks[index].load(std::memory_order_relaxed));
    }
}

/// Guest bytes read for a texture upload (volume only).
inline void CountRead(Site site, u64 size) {
    if (!Enabled()) {
        return;
    }
    const size_t index = static_cast<size_t>(site);
    Detail::g_counters.checks[index].fetch_add(1, std::memory_order_relaxed);
    Detail::g_counters.read_bytes[index].fetch_add(size, std::memory_order_relaxed);
}

/// A guest write overlaps GPU-written bytes the buffer cache holds (their download is dropped).
inline void NoteGpuModifiedWrite(Site site, DAddr addr, u64 size) {
    if (!Enabled()) {
        return;
    }
    const size_t index = static_cast<size_t>(site);
    Detail::g_counters.checks[index].fetch_add(1, std::memory_order_relaxed);
    Detail::g_counters.bytes[index].fetch_add(size, std::memory_order_relaxed);
    const u64 count = Detail::g_counters.hazards[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 48 || count % 2000 == 0 || Verbose()) {
        u64 unit = 0;
        const u64 written = Detail::NewestWrite(addr, size, unit);
        LOG_INFO(HW_GPU,
                 "nx2 hazard: {} [{:#x}, +{:#x}), guest write epoch {}, back list epoch {} (#{})",
                 SiteNames[index], addr, size, written,
                 g_back_epoch.load(std::memory_order_relaxed), count);
    }
}

/// One line per site that saw anything (called with the present statistics).
inline void LogSummary() {
    if (!Enabled()) {
        return;
    }
    for (size_t i = 0; i < static_cast<size_t>(Site::Count); ++i) {
        const u64 checks = Detail::g_counters.checks[i].load(std::memory_order_relaxed);
        if (checks != 0) {
            LOG_INFO(HW_GPU,
                     "nx2 hazard total at present {}: {}: {} events, {} hazards ({} KiB), "
                     "{} MiB read",
                     CurrentEpoch(), SiteNames[i], checks,
                     Detail::g_counters.hazards[i].load(std::memory_order_relaxed),
                     Detail::g_counters.bytes[i].load(std::memory_order_relaxed) >> 10,
                     Detail::g_counters.read_bytes[i].load(std::memory_order_relaxed) >> 20);
        }
    }
}

} // namespace VideoCommon::Nx2Hazard
