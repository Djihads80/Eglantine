// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;
inline std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions> g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        if (region.load() == start) {
            return;
        }
    }
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = 0;
        if (region.compare_exchange_strong(expected, start)) {
            return;
        }
    }
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = start;
        region.compare_exchange_strong(expected, 0);
    }
}

/// A new emulation session starts: no queue has a region yet.
inline void Nx2ClearSemaphoreRegions() {
    for (auto& region : g_nx2_semaphore_regions) {
        region.store(0);
    }
}

// Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and
// layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the
// GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover;
// the GPU memory manager calls Nx2BackSparseWrite before each write.
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size)>;
inline std::recursive_mutex g_nx2_sparse_backing_mutex;
inline Nx2SparseBackingHandler g_nx2_sparse_backing;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    g_nx2_sparse_backing = std::move(handler);
    g_nx2_sparse_low.store(~u64{0});
    g_nx2_sparse_high.store(0);
}

/// Widens the span that writes are checked against (a new lazily backed region).
inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    u64 low = g_nx2_sparse_low.load();
    while (gpu_address < low && !g_nx2_sparse_low.compare_exchange_weak(low, gpu_address)) {
    }
    u64 high = g_nx2_sparse_high.load();
    while (gpu_address + size > high &&
           !g_nx2_sparse_high.compare_exchange_weak(high, gpu_address + size)) {
    }
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size);
    }
}

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
};
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {size, block_size,
                                       std::vector<bool>((size + block_size - 1) / block_size)};
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address); it != g_nx2_sparse_state.end()) {
        it->second.written[block] = true;
    }
}

/// True when [gpu_address, +size) lies in one sparse region.
inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second.size;
}

/// The GPU renders into [gpu_address, +size): count its blocks as written, so images made there
/// later get memory (their contents live in the host image, not in guest memory).
inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    for (auto& [start, region] : g_nx2_sparse_state) {
        const u64 end = start + region.size;
        if (gpu_address >= end || gpu_address + size <= start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = ((std::min)(gpu_address + size, end) - 1 - start) / region.block_size;
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

/// True when [gpu_address, +size) lies in one sparse region and none of it has been written.
inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    if (gpu_address + size > start + region.size) {
        return false;
    }
    const u64 last = (gpu_address + size - 1 - start) / region.block_size;
    for (u64 block = (gpu_address - start) / region.block_size; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

/// GPU VA in the first page that mirrors a release at `address`, or 0.
inline u64 Nx2SemaphoreMirror(u64 address) {
    for (const auto& region : g_nx2_semaphore_regions) {
        const u64 start = region.load(std::memory_order_relaxed);
        if (start == 0) {
            continue;
        }
        if (address >= start + 0x1000 && address < start + 0x2000) {
            return address - 0x1000;
        }
    }
    return 0;
}

/// Channels suspended on a semaphore acquire. A console channel polls and sees every release;
/// Eden writes releases in bursts when fences signal and resumes waiters only between command
/// lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.
class Nx2SemaphoreWaiters {
public:
    // Puller GpuSemaphoreOperation values.
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    /// A GPU write of `value` to the u32 at `device_address`.
    void OnWrite(u64 device_address, u32 value) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        for (auto& w : waiters) {
            if (w.address == device_address && !w.latched && Matches(w, value)) {
                w.latched = true;
            }
        }
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
};

} // namespace Tegra
