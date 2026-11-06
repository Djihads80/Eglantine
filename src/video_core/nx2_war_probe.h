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
#include "video_core/nx2_perf.h"

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
        }
    }
}

} // namespace VideoCommon::Nx2WarProbe
