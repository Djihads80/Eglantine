// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

// Nx2Perf inflight_write_wait / inflight_read_exact: with frames_ahead, vi2 replies to present N
// after frame N-1's fence and opens an epoch for fence N. Core memory write-protects the pages the
// back may still read for frame N (the war_guard_probe arm); the guest's first write of one, or of
// a tracker-protected page, waits on a guest core until the back passes fence N and closes the
// epoch. inflight_read_exact: while the epoch is open, a guest read the buffer cache serves from a
// preflush block whose last GPU writer is in flight waits for fence N's host value. Shared by core
// memory (traps, waits), the buffer cache (writer stamps, preflush reads) and vi2 (opening, log).
namespace VideoCommon::Nx2Inflight {

inline bool WriteWait() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::FramesAhead) &&
                           Nx2Perf::On(Nx2Perf::Opt::InflightWriteWait);
    return on;
}

inline bool ReadExact() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::FramesAhead) &&
                           Nx2Perf::On(Nx2Perf::Opt::InflightReadExact);
    return on;
}

/// Whether epochs can open at all; false keeps every trap and read path as it was.
inline bool Any() {
    static const bool on = WriteWait() || ReadExact();
    return on;
}

struct Epoch {
    std::atomic<u64> open_seq{0}; ///< Sequence of the open epoch, 0 while none is open
    std::atomic<u64> seq{0};      ///< Epochs opened so far
    std::atomic<u32> fence_id{~0U};
    std::atomic<u32> fence_value{0};
    std::atomic<u32> prev_id{~0U};
    std::atomic<u32> prev_value{0};
    std::atomic<bool> guarded{false}; ///< Pages may still be write-protected by an epoch's arm
    std::atomic<u64> fence_reached_seq{0}; ///< Last epoch whose fence the back passed
    /// Back: one stamp per GPU write of a preflush block (BufferCache::QueueGpuWrite).
    std::atomic<u64> write_stamp{0};
    std::atomic<u64> passed_stamp{0}; ///< write_stamp when the back passed passed_seq's fence
    std::atomic<u64> passed_seq{0};
    /// Blocks written with a stamp above this are frame N's writers; 0 when unknown (every
    /// written block counts as in flight).
    std::atomic<u64> prev_fence_stamp{0};

    // Window counters, read and cleared by vi2's per-second log.
    std::atomic<u64> opened{}, closed_by_write{}, closed_by_fence{}, closed_by_present{};
    std::atomic<u64> escapes{}, host_writes{}, write_waits{}, write_wait_ns{}, max_wait_ns{};
    std::atomic<u64> read_waits{}, read_wait_ns{}, read_escapes{}, preflush_reads{};
    std::atomic<u64> failed_opens{};
    // Verify: (a) bank pages the guest changed before the back passed the epoch's fence, (b)
    // preflush-served reads that differ from the bytes after their in-flight writer landed.
    std::atomic<u64> bank_violations{}, read_compared{}, read_differs{}, read_compare_dropped{};
    std::atomic<u64> escape_logs{}, verify_logs{};
};

inline Epoch& Get() {
    static Epoch epoch;
    return epoch;
}

/// Buffer cache -> core memory, on the reading thread: what the last OnCPURead served.
struct ReadNote {
    bool preflush_served = false; ///< Served from a preflush mark while an epoch was open
    bool in_flight = false;       ///< A block of it was written after the previous fence
};
inline thread_local ReadNote read_note{};

/// Verify (b): reads compared with their bytes after fence N lands, a fixed pool.
struct ReadCompare {
    static constexpr size_t MaxBytes = 64;
    std::atomic<u32> busy{0};
    u64 address = 0;
    u32 size = 0;
    const u8* host = nullptr;
    std::array<u8, MaxBytes> bytes{};
};
inline constexpr size_t NumReadCompares = 64;

inline std::array<ReadCompare, NumReadCompares>& ReadCompares() {
    static std::array<ReadCompare, NumReadCompares> pool;
    return pool;
}

} // namespace VideoCommon::Nx2Inflight

// Nx2Perf war_guard_probe (measurement only): how long after vi2's Ocarina reply the guest first
// touches memory the back may still read for the frame it just presented (t_w), and how many
// native uniform bank pages the guest rewrites before the next frame's first bind of them. No
// access is blocked or reordered: core memory write-protects the pages the tracker left writable
// (CPU-written since their upload) after the reply and gives each one back its permission on its
// first write. Shared by core memory (traps), the buffer cache (bank pages) and vi2 (arming, log).
namespace VideoCommon::Nx2WarProbe {

inline bool On() {
    static const bool on = Nx2Perf::On(Nx2Perf::Opt::WarGuardProbe);
    return on;
}

enum class Category : u32 {
    Cached,      ///< Write to a page the tracker protects (HandleRasterizerWrite)
    Unprotected, ///< First write to a CPU-written page the probe protected after the reply
    Bank,        ///< Either kind of write to a native uniform bank page
    Read,        ///< Read trap of a GPU-written page (HandleRasterizerDownload)
    Count,
};

inline constexpr size_t NumCategories = static_cast<size_t>(Category::Count);
inline constexpr size_t PageBits = 12;
inline constexpr size_t PageSize = size_t{1} << PageBits;
inline constexpr size_t MaxBankPages = 4096;
inline constexpr size_t SnapshotPages = 1024;

struct BankPage {
    u64 device_page; ///< Device address >> PageBits
    u64 cpu_page;    ///< Guest virtual address >> PageBits, 0 without a CPU mapping
    const u8* host;  ///< Backing memory of the page, nullptr when unmapped
};

struct State {
    // Buffer cache (back thread, TickFrame) -> vi2: bank pages bound in the last two frames,
    // sorted by device page.
    std::mutex export_mutex;
    std::array<BankPage, MaxBankPages> exported{};
    size_t exported_count = 0;
    u64 export_overflow = 0;

    // vi2 (after its wait) -> back (first bind of a bank after it): the bank pages' guest bytes.
    std::mutex snapshot_mutex;
    std::unique_ptr<u8[]> snapshot_bytes;
    std::array<u64, SnapshotPages> snapshot_device_page{};
    std::array<u8, SnapshotPages> snapshot_pending{};
    size_t snapshot_count = 0;
    /// inflight_write_wait: the epoch opened after this snapshot, 0 when none did.
    u64 snapshot_epoch = 0;

    // The armed frame: vi2 bumps frame and clears reply_ns when it arms, sets reply_ns when it
    // replies; the first guest trap after that claims the frame.
    std::atomic<u64> frame{0};
    std::atomic<s64> reply_ns{0};
    std::atomic<u64> first_claim{0};
    std::atomic<u64> first_done{0};
    s64 first_ns = 0;
    Category first_category = Category::Cached;

    // Window counters, read and cleared by vi2's log.
    std::array<std::atomic<u64>, NumCategories> traps{};
    std::atomic<u64> host_traps{};      ///< Traps from threads that are not guest cores
    std::atomic<u64> pre_reply_traps{}; ///< Guest traps between the arm and the reply
    std::atomic<u64> bank_compared{};
    std::atomic<u64> bank_changed{};
    std::atomic<u64> verify_differences{};
};

inline State& Get() {
    static State state;
    return state;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A trapped access of a guarded page; guest_core is false for host (service) threads.
inline void NoteTrap(Category category, bool guest_core) {
    State& state = Get();
    if (!guest_core) {
        state.host_traps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.traps[static_cast<size_t>(category)].fetch_add(1, std::memory_order_relaxed);
    const u64 frame = state.frame.load(std::memory_order_acquire);
    if (frame == 0 || state.first_done.load(std::memory_order_acquire) == frame) {
        return;
    }
    const s64 reply = state.reply_ns.load(std::memory_order_acquire);
    if (reply == 0) {
        state.pre_reply_traps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u64 claimed = state.first_claim.load(std::memory_order_relaxed);
    if (claimed == frame ||
        !state.first_claim.compare_exchange_strong(claimed, frame, std::memory_order_acq_rel)) {
        return;
    }
    state.first_ns = NowNs() - reply;
    state.first_category = category;
    state.first_done.store(frame, std::memory_order_release);
}

/// The back's first bind of a bank after vi2's snapshot: counts the bank pages whose guest bytes
/// changed since then (direct evidence that the guest rewrites data a frame still in flight
/// would read). get_pointer(device address) returns the page's backing memory or nullptr.
template <typename GetPointer>
void CompareBank(u64 device_addr, u64 size, GetPointer&& get_pointer) {
    State& state = Get();
    std::scoped_lock lk{state.snapshot_mutex};
    if (state.snapshot_count == 0 || size == 0) {
        return;
    }
    const u64 first = device_addr >> PageBits;
    const u64 last = (device_addr + (std::min<u64>)(size, 0x10000) - 1) >> PageBits;
    const u64* const begin = state.snapshot_device_page.data();
    const u64* const end = begin + state.snapshot_count;
    for (const u64* it = std::lower_bound(begin, end, first); it != end && *it <= last; ++it) {
        const size_t index = static_cast<size_t>(it - begin);
        if (state.snapshot_pending[index] == 0) {
            continue;
        }
        state.snapshot_pending[index] = 0;
        const u8* const now = get_pointer(*it << PageBits);
        if (now == nullptr) {
            continue;
        }
        state.bank_compared.fetch_add(1, std::memory_order_relaxed);
        if (std::memcmp(now, state.snapshot_bytes.get() + index * PageSize, PageSize) != 0) {
            state.bank_changed.fetch_add(1, std::memory_order_relaxed);
            // inflight_write_wait: the back binds the page before passing the epoch's fence, so
            // this is frame N's read of a page the guest rewrote with the epoch open.
            auto& epoch = Nx2Inflight::Get();
            if (state.snapshot_epoch != 0 &&
                epoch.fence_reached_seq.load(std::memory_order_acquire) < state.snapshot_epoch) {
                const u64 count =
                    epoch.bank_violations.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf inflight_write_wait: bank page {:#x} changed before the "
                              "back passed epoch {}'s fence ({} so far)",
                              *it << PageBits, state.snapshot_epoch, count);
                }
            }
        }
    }
}

} // namespace VideoCommon::Nx2WarProbe
