// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

// Nx2Perf bank_dirty_skip: the native uniform bank compare (buffer_cache.h) skips a page whose
// write sequence has not moved since its last compare. Every physical page has a sequence word
// (bit 0: a bank page). Device memory and GPU memory manager write hooks, the trap of a guest page
// the compare write-protected (core/memory.cpp) and the tracker's write path bump it. A hook runs
// before its write lands, so a page compared because it moved is compared again in a later frame.
namespace VideoCommon::Nx2BankDirty {

inline constexpr u64 PageBits = 12;
inline constexpr size_t NumPhysPages = size_t{1} << 23; ///< 35-bit physical layout
inline constexpr u32 BankBit = 1;
inline constexpr u32 SeqStep = 2;
inline constexpr size_t MaxPages = 17; ///< An unaligned 64 KiB bank spans 17 pages
static_assert(MaxPages <= 32);

inline bool On() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::BankDirtySkip);
    return on;
}

/// Set once the table exists; the host write hooks test it.
inline std::atomic<bool> g_on{false};

enum class Source : u8 { Trap, Tracker, Hook, Count };

/// Per physical page: sequence << 1 | BankBit.
struct Table {
    std::unique_ptr<std::atomic<u32>[]> seq;
    size_t pages = 0;

    void Init(size_t count) {
        seq = std::make_unique<std::atomic<u32>[]>(count);
        pages = count;
    }

    /// The page's word, marking it a bank page.
    u32 Watch(u64 phys) {
        std::atomic<u32>& word = seq[phys];
        const u32 value = word.load(std::memory_order_acquire);
        if ((value & BankBit) != 0) {
            return value;
        }
        return word.fetch_or(BankBit, std::memory_order_acq_rel) | BankBit;
    }

    u32 Load(u64 phys) const {
        return seq[phys].load(std::memory_order_acquire);
    }

    /// A write of the page is about to land: moves a bank page's sequence.
    bool Mark(u64 phys) {
        if (phys >= pages) {
            return false;
        }
        std::atomic<u32>& word = seq[phys];
        if ((word.load(std::memory_order_relaxed) & BankBit) == 0) {
            return false;
        }
        word.fetch_add(SeqStep, std::memory_order_acq_rel);
        return true;
    }
};

/// One bank shadow's pages (index = page of the bank, as in its initialized_mask).
struct ShadowPages {
    std::array<u32, MaxPages> seq{};
    std::array<u32, MaxPages> phys{}; ///< Physical page + 1 at the last compare, 0 for none
    u32 watched{};   ///< Write-protected (or tracker-protected) since the last compare
    u32 probation{}; ///< Moved at a compare: compared again in a later frame
    u32 moved{};     ///< Moved at this pass's compares
    u64 probation_tick{~u64{0}};
    bool force = true; ///< Compare every page (new shadow, or the bank was GPU-modified)
};

/// Whether page idx (physical page phys) may skip its compare.
inline bool CanSkip(const Table& table, const ShadowPages& sp, u32 idx, u64 phys) {
    if (sp.force || idx >= MaxPages || phys >= table.pages) {
        return false;
    }
    const u32 bit = 1U << idx;
    if ((sp.watched & bit) == 0 || (sp.probation & bit) != 0 || sp.phys[idx] != phys + 1) {
        return false;
    }
    return table.Load(phys) == sp.seq[idx];
}

/// Before page idx's compare: whether its guest write protection must be (re-)armed.
inline bool BeforeCompare(Table& table, ShadowPages& sp, u32 idx, u64 phys) {
    if (idx >= MaxPages || phys >= table.pages) {
        return false;
    }
    const u32 bit = 1U << idx;
    const u32 now = table.Watch(phys);
    const bool same = (sp.watched & bit) != 0 && sp.phys[idx] == phys + 1 && sp.seq[idx] == now;
    sp.seq[idx] = now;
    sp.phys[idx] = static_cast<u32>(phys + 1);
    if (same) {
        return false;
    }
    sp.moved |= bit;
    sp.watched &= ~bit;
    return true;
}

/// The arm's answer for page idx.
inline void Armed(ShadowPages& sp, u32 idx, bool ok) {
    if (ok && idx < MaxPages) {
        sp.watched |= 1U << idx;
    }
}

/// After a pass over the bank's pages in frame frame_tick.
inline void EndPass(ShadowPages& sp, u64 frame_tick) {
    const u32 carry = sp.probation_tick == frame_tick ? sp.probation : 0;
    sp.probation = carry | sp.moved;
    if (sp.moved != 0) {
        sp.probation_tick = frame_tick;
    }
    sp.moved = 0;
    sp.force = false;
}

using DevicePointerFn = const u8* (*)(const void* device, u64 device_addr);

enum Counter : size_t {
    Avoided,
    Compared,
    Traps,
    TrackerMarks,
    HookMarks,
    Rearms,
    ArmFailures,
    NoMark,
    NumCounters,
};

struct State {
    std::mutex init_mutex;
    std::atomic<bool> ready{false};
    Table table;
    std::atomic<const void*> device{nullptr};
    std::atomic<DevicePointerFn> device_pointer{nullptr};
    std::atomic<uintptr_t> physical_base{0};
    std::array<std::atomic<u64>, NumCounters> c{};
    std::atomic<u64> no_mark_total{};
    s64 last_log_ns{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void Add(Counter counter, u64 amount = 1) {
    Get().c[counter].fetch_add(amount, std::memory_order_relaxed);
}

/// Allocates the table once (switch on only).
inline void EnsureInit() {
    State& s = Get();
    if (s.ready.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{s.init_mutex};
    if (s.ready.load(std::memory_order_relaxed)) {
        return;
    }
    s.table.Init(NumPhysPages);
    s.ready.store(true, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
}

/// Device memory (the GPU's manager) at bind time.
inline void RegisterDevice(const void* device, DevicePointerFn fn, uintptr_t physical_base) {
    if (!On()) {
        return;
    }
    EnsureInit();
    State& s = Get();
    s.physical_base.store(physical_base, std::memory_order_relaxed);
    s.device.store(device, std::memory_order_relaxed);
    s.device_pointer.store(fn, std::memory_order_release);
}

inline u64 PhysOf(const u8* host) {
    return (reinterpret_cast<uintptr_t>(host) -
            Get().physical_base.load(std::memory_order_relaxed)) >>
           PageBits;
}

/// Core memory: a write of physical page phys is about to land (a trap or the tracker's path).
inline void Mark(u64 phys, Source source) {
    State& s = Get();
    if (!s.ready.load(std::memory_order_acquire) || !s.table.Mark(phys)) {
        return;
    }
    Add(source == Source::Trap ? Traps : TrackerMarks);
}

inline void HostWriteSlow(u64 device_addr, u64 size) {
    State& s = Get();
    const DevicePointerFn fn = s.device_pointer.load(std::memory_order_acquire);
    if (size == 0 || fn == nullptr) {
        return;
    }
    const void* const device = s.device.load(std::memory_order_relaxed);
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        if (const u8* const host = fn(device, page << PageBits)) {
            if (s.table.Mark(PhysOf(host))) {
                Add(HookMarks);
            }
        }
    }
}

/// Device memory and the GPU's memory manager: a host write is about to land.
inline void HostWrite(u64 device_addr, u64 size) {
    if (g_on.load(std::memory_order_relaxed)) [[unlikely]] {
        HostWriteSlow(device_addr, size);
    }
}

/// Verify: a skipped page's bytes differ from its shadow.
inline void NoteNoMark(u64 bank, u32 page) {
    const u64 count = Get().no_mark_total.fetch_add(1, std::memory_order_relaxed) + 1;
    Add(NoMark);
    if (count <= 64) {
        LOG_ERROR(HW_GPU,
                  "Nx2Perf bank_dirty_skip: page changed with no mark (bank {:#x}, page {}) "
                  "({} so far)",
                  bank, page, count);
    }
}

/// The back, once a frame: the counters of the last second.
inline void MaybeLog() {
    State& s = Get();
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (s.last_log_ns == 0) {
        s.last_log_ns = now;
        return;
    }
    if (now - s.last_log_ns < 1'000'000'000) {
        return;
    }
    s.last_log_ns = now;
    std::array<u64, NumCounters> v{};
    for (size_t i = 0; i < NumCounters; ++i) {
        v[i] = s.c[i].exchange(0, std::memory_order_relaxed);
    }
    LOG_INFO(HW_GPU,
             "Nx2Perf bank_dirty_skip: per second compares avoided {} done {}, traps {}, tracker "
             "marks {}, hook marks {}, re-arms {} (failed {}), no-mark differences {} ({} total)",
             v[Avoided], v[Compared], v[Traps], v[TrackerMarks], v[HookMarks], v[Rearms],
             v[ArmFailures], v[NoMark], s.no_mark_total.load(std::memory_order_relaxed));
}

} // namespace VideoCommon::Nx2BankDirty
