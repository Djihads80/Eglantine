// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/thread.h"
#include "video_core/nx2_perf.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};

// The same, one bit per 1 MiB: the split front's early MME DMA reads fall back to the drain for
// any read in a region the GPU has written. A queued dispatch can write a page of such a region
// for the first time before its bit is set (Ocarina's indirect draw arguments did).
inline constexpr u64 Nx2WrittenRegionBits = 20;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenRegionBits)) / 64>
    g_nx2_gpu_written_regions{};

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    {
        const u64 last_region =
            std::min<u64>((device_address + size - 1) >> Nx2WrittenRegionBits,
                          g_nx2_gpu_written_regions.size() * 64 - 1);
        for (u64 region = device_address >> Nx2WrittenRegionBits; region <= last_region;
             ++region) {
            auto& word = g_nx2_gpu_written_regions[region / 64];
            const u64 bit = u64{1} << (region % 64);
            if ((word.load(std::memory_order_relaxed) & bit) == 0) {
                word.fetch_or(bit, std::memory_order_relaxed);
            }
        }
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last;) {
        const u64 first_bit = page % 64;
        const u64 bits = std::min<u64>(64 - first_bit, last - page + 1);
        const u64 mask = (bits == 64 ? ~u64{0} : (u64{1} << bits) - 1) << first_bit;
        auto& word = g_nx2_gpu_written_pages[page / 64];
        if ((word.load(std::memory_order_relaxed) & mask) != mask) {
            word.fetch_or(mask, std::memory_order_relaxed);
        }
        page += bits;
    }
}

// GPU-written device pages the front has read command lists from (same layout as above). The
// buffer cache copies these pages into host memory right after the compute or copy work that
// writes them and submits, so the front reads the copy once the GPU is done instead of draining
// the split and flushing the caches (BufferCache::Nx2ReadListCopy).
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_command_list_pages{};
inline std::atomic<bool> g_nx2_has_command_list_pages{false};

// Shaders in graphics pipeline stages that write storage buffers or command list pages.
// Checked on the split GPU thread's front to identify writer draws and note them with split.NoteWriter().
// Sized to 65,536 bits (1,024 64-bit words, 8 KiB), indexed via a fast multiplicative hash of the
// 32-bit low shader address (which is aligned to 128/256 bytes in native Switch 2 drivers).
inline constexpr size_t Nx2WriterShaderBits = 65536;
inline std::array<std::atomic<u64>, Nx2WriterShaderBits / 64> g_nx2_graphics_writer_shaders{};
inline std::atomic<bool> g_nx2_has_graphics_writer_shaders{false};

inline void Nx2MarkGraphicsWriterShader(u32 addr_low) {
    if (addr_low == 0) {
        return;
    }
    const u32 hash = (addr_low >> 6) * 0x9e3779b9u;
    const u32 bit_index = hash & (Nx2WriterShaderBits - 1);
    auto& word = g_nx2_graphics_writer_shaders[bit_index / 64];
    const u64 bit = u64{1} << (bit_index % 64);
    if ((word.load(std::memory_order_relaxed) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_release);
        g_nx2_has_graphics_writer_shaders.store(true, std::memory_order_release);
    }
}

[[nodiscard]] inline bool Nx2IsGraphicsWriterShader(u32 addr_low) {
    if (addr_low == 0 || !g_nx2_has_graphics_writer_shaders.load(std::memory_order_acquire)) {
        return false;
    }
    const u32 hash = (addr_low >> 6) * 0x9e3779b9u;
    const u32 bit_index = hash & (Nx2WriterShaderBits - 1);
    const u64 word = g_nx2_graphics_writer_shaders[bit_index / 64].load(std::memory_order_relaxed);
    return (word & (u64{1} << (bit_index % 64))) != 0;
}

inline void Nx2MarkCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return;
    }
    auto& word = g_nx2_command_list_pages[page / 64];
    const u64 bit = u64{1} << (page % 64);
    if ((word.load(std::memory_order_relaxed) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_relaxed);
        g_nx2_has_command_list_pages.store(true, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return false;
    }
    return (g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

// Nx2Perf MacroReadWriterSet: graphics draws the back proved write no guest memory, so the
// split front's early MME DMA reads need not wait for them. A proof holds for the exact program
// addresses a draw binds (Nx2DrawPrograms, built from the registers on both threads) and for one
// program epoch: the epoch moves whenever the shader cache drops a program, and whenever a write
// that the shader cache only applies later (CPU writes into GPU-modified buffers, accumulated
// invalidations) reaches a page holding proven code. A proof from an older epoch proves nothing,
// so an address that later holds other code is never trusted. Epoch 0 is never valid.
inline std::atomic<u64> g_nx2_code_epoch{1};

inline void Nx2BumpCodeEpoch() {
    g_nx2_code_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// Device pages (4 KiB) holding code of a proven program (same layout as g_nx2_gpu_written_pages).
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_proven_code_pages{};
inline std::atomic<bool> g_nx2_has_proven_code{false};

inline void Nx2MarkProvenCode(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_proven_code_pages.size() * 64 - 1);
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        auto& word = g_nx2_proven_code_pages[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word.load(std::memory_order_relaxed) & bit) == 0) {
            word.fetch_or(bit, std::memory_order_release);
        }
    }
    g_nx2_has_proven_code.store(true, std::memory_order_release);
}

[[nodiscard]] inline bool Nx2IsProvenCode(u64 device_address, u64 size) {
    if (size == 0 || !g_nx2_has_proven_code.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_proven_code_pages.size() * 64 - 1);
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        if ((g_nx2_proven_code_pages[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1) {
            return true;
        }
    }
    return false;
}

/// A write whose invalidation the shader cache applies later reached [device_address, +size).
inline void Nx2NoteDeferredCodeWrite(u64 device_address, u64 size) {
    if (Nx2IsProvenCode(device_address, size)) {
        Nx2BumpCodeEpoch();
    }
}

/// The programs a graphics draw binds, as Maxwell3D::Nx2DrawPrograms computes them from the
/// registers: per program slot, its GPU address (0 when disabled).
struct Nx2DrawPrograms {
    std::array<u64, 6> addresses{};
    u64 memory_id{};
    u64 map_serial{};
    bool native{};

    bool operator==(const Nx2DrawPrograms&) const = default;
};

struct Nx2NonWriterSet {
    struct Hash {
        size_t operator()(const Nx2DrawPrograms& key) const noexcept {
            u64 hash = key.memory_id * 0x9e3779b97f4a7c15ULL ^ key.map_serial ^ (key.native ? 1 : 0);
            for (const u64 address : key.addresses) {
                hash = (hash ^ address) * 0xff51afd7ed558ccdULL;
                hash ^= hash >> 32;
            }
            return static_cast<size_t>(hash);
        }
    };
    std::mutex mutex;
    std::map<u64, std::vector<std::pair<Nx2DrawPrograms, u64>>> buckets; ///< hash -> (key, epoch)
    std::atomic<u64> version{1}; ///< Moves on every insert (the front caches its lookups)
};

inline Nx2NonWriterSet& Nx2NonWriters() {
    static Nx2NonWriterSet set;
    return set;
}

/// Whether the back proved, in program epoch `epoch`, that a draw binding `programs` writes no
/// guest memory.
[[nodiscard]] inline bool Nx2IsNonWriterDraw(const Nx2DrawPrograms& programs, u64 epoch) {
    auto& set = Nx2NonWriters();
    const u64 hash = Nx2NonWriterSet::Hash{}(programs);
    std::scoped_lock lock{set.mutex};
    const auto it = set.buckets.find(hash);
    if (it == set.buckets.end()) {
        return false;
    }
    for (const auto& [key, proven_epoch] : it->second) {
        if (key == programs) {
            return proven_epoch == epoch;
        }
    }
    return false;
}

inline void Nx2InsertNonWriterDraw(const Nx2DrawPrograms& programs, u64 epoch) {
    auto& set = Nx2NonWriters();
    const u64 hash = Nx2NonWriterSet::Hash{}(programs);
    {
        std::scoped_lock lock{set.mutex};
        auto& bucket = set.buckets[hash];
        bool found = false;
        for (auto& [key, proven_epoch] : bucket) {
            if (key == programs) {
                proven_epoch = epoch;
                found = true;
                break;
            }
        }
        if (!found) {
            bucket.emplace_back(programs, epoch);
        }
    }
    set.version.fetch_add(1, std::memory_order_release);
}

/// A new emulation session starts: no page has been written by its GPU yet.
inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_command_list_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_command_list_pages.store(false, std::memory_order_relaxed);
    for (auto& word : g_nx2_graphics_writer_shaders) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_graphics_writer_shaders.store(false, std::memory_order_relaxed);
    Nx2BumpCodeEpoch(); // No proof of the last session holds
    {
        auto& set = Nx2NonWriters();
        std::scoped_lock lock{set.mutex};
        set.buckets.clear();
    }
}

// Query results written when their host work completes (counter reports, delayed fence
// releases): per 16-byte slot hash, how many result writes the back queued and how many ran.
// Equal counts mean the words in memory are final, so the split's front can read them without
// draining the back (lichii #179, 166c21d96c). A hash collision only makes a read wait longer.
inline constexpr u64 Nx2AsyncReportSlots = u64{1} << 16;
inline std::array<std::atomic<u32>, Nx2AsyncReportSlots> g_nx2_async_reports_issued{};
inline std::array<std::atomic<u32>, Nx2AsyncReportSlots> g_nx2_async_reports_completed{};

/// A new emulation session starts: counter reports the previous one abandoned are not pending.
inline void Nx2ClearAsyncReports() {
    for (auto& count : g_nx2_async_reports_issued) {
        count.store(0, std::memory_order_relaxed);
    }
    for (auto& count : g_nx2_async_reports_completed) {
        count.store(0, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline u64 Nx2AsyncReportSlot(u64 device_address) {
    return (device_address >> 4) % Nx2AsyncReportSlots;
}

[[nodiscard]] inline bool Nx2AsyncReportsSettled(u64 device_address, u64 size) {
    for (u64 slot = device_address >> 4; slot <= (device_address + size - 1) >> 4; ++slot) {
        const u64 index = slot % Nx2AsyncReportSlots;
        if (g_nx2_async_reports_completed[index].load(std::memory_order_acquire) !=
            g_nx2_async_reports_issued[index].load(std::memory_order_acquire)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool Nx2IsGpuWrittenRegion(u64 device_address) {
    const u64 region = device_address >> Nx2WrittenRegionBits;
    if (region >= g_nx2_gpu_written_regions.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_regions[region / 64].load(std::memory_order_relaxed) >>
            (region % 64)) &
           1;
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

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
// the GPU memory manager calls Nx2BackSparseWrite before each write, with the bytes it writes
// (blocks nobody wrote that only get zeros need no memory: they read zero already).
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size, const u8* data)>;
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

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size, const void* data) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size, static_cast<const u8*>(data));
    }
}

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
    /// Once the game maps its own memory into the region (nx2:gpu 216): which tiles it maps.
    /// Only those hold data then (writes elsewhere are dropped, as on the console).
    std::vector<bool> mapped;
};

/// The granularity of the game's sparse tiles (nx2:gpu 216 maps 64 KiB pages).
inline constexpr u64 Nx2SparseTileSize = 64ULL << 10;
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {size, block_size,
                                       std::vector<bool>((size + block_size - 1) / block_size)};
}

/// An initially GPU-only pool now has explicit tiles. Let the renderer bind only resident tiles,
/// while preserving the written/mapped state of pools that were sparse from creation.
inline void Nx2SparseRegionTiled(u64 gpu_address, u64 size, u64 block_size) {
    Nx2AddSparseSpan(gpu_address, size);
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (!g_nx2_sparse_state.contains(gpu_address)) {
        g_nx2_sparse_state[gpu_address] = {
            size, block_size, std::vector<bool>((size + block_size - 1) / block_size)};
    }
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

/// A new emulation session: the previous game's regions are gone (nvdrv's Module clears them).
inline void Nx2ClearSparseRegions() {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.clear();
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address); it != g_nx2_sparse_state.end()) {
        it->second.written[block] = true;
    }
}

/// True unless the block of the region at region_address is known to hold nothing (no write from
/// the CPU or the GPU yet).
inline bool Nx2SparseBlockIsWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    const auto it = g_nx2_sparse_state.find(region_address);
    return it == g_nx2_sparse_state.end() || block >= it->second.written.size() ||
           it->second.written[block];
}

/// The game maps its own memory over [offset, +size) of the region at region_address (mapped), or
/// unmaps it there.
inline void Nx2SparseTilesMapped(u64 region_address, u64 offset, u64 size, bool mapped) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    const auto it = g_nx2_sparse_state.find(region_address);
    if (it == g_nx2_sparse_state.end() || size == 0) {
        return;
    }
    auto& region = it->second;
    if (region.mapped.empty()) {
        region.mapped.resize((region.size + Nx2SparseTileSize - 1) / Nx2SparseTileSize);
    }
    const u64 last = (std::min)((offset + size - 1) / Nx2SparseTileSize, u64{region.mapped.size() - 1});
    for (u64 tile = offset / Nx2SparseTileSize; tile <= last; ++tile) {
        region.mapped[tile] = mapped;
    }
}

/// The parts of [gpu_address, +size), which lies in one sparse region, that can hold data: the
/// tiles the game maps there if it maps any, otherwise the blocks written. As (offset from
/// gpu_address, size) runs.
inline std::vector<std::pair<u64, u64>> Nx2SparseDataRanges(u64 gpu_address, u64 size) {
    std::vector<std::pair<u64, u64>> ranges;
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return ranges;
    }
    --it;
    const auto& [start, region] = *it;
    if (size == 0 || gpu_address + size > start + region.size) {
        return ranges;
    }
    const bool by_tile = !region.mapped.empty();
    const u64 unit = by_tile ? Nx2SparseTileSize : region.block_size;
    const std::vector<bool>& present = by_tile ? region.mapped : region.written;
    const u64 begin = gpu_address - start;
    const u64 end = begin + size;
    for (u64 index = begin / unit; index * unit < end && index < present.size(); ++index) {
        if (!present[index]) {
            continue;
        }
        const u64 run_begin = (std::max)(index * unit, begin) - begin;
        const u64 run_end = (std::min)((index + 1) * unit, end) - begin;
        if (!ranges.empty() && ranges.back().first + ranges.back().second == run_begin) {
            ranges.back().second += run_end - run_begin;
        } else {
            ranges.emplace_back(run_begin, run_end - run_begin);
        }
    }
    return ranges;
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

/// pending_wake: the GPU thread's 1 ms sleep while a channel waits on an acquire ends at once
/// when a command is queued or a release latches a waiter. The timeout bounds a missed wake.
namespace Nx2PendingWake {
enum Reason : u32 { ByPush = 1, ByLatch = 2 };

inline bool On() {
    static const bool on = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PendingWake);
    return on;
}

inline Common::Event wake_event;
inline std::atomic<bool> front_sleeping{};
inline std::atomic<u32> wake_reasons{};
inline std::atomic<u64> push_seq{};
inline std::atomic<u64> latch_seq{};
inline std::atomic<s64> last_latch_ns{}; ///< Steady clock of the last latch (0: consumed)
/// EDEN_NX2_PERF_VERIFY: resumes after a wake with the word unmet and nothing latched.
inline std::atomic<u64> verify_unmet{};
/// GPU thread only: the next pending poll follows a wake (Verify checks its resumes).
inline bool verify_resume{};

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Bumps `seq`, then wakes the front if it sleeps. The front sets front_sleeping before it
/// re-reads the counters (both seq_cst): it sees the bump or this sees the flag.
inline void Wake(std::atomic<u64>& seq, u32 reason) {
    seq.fetch_add(1, std::memory_order_seq_cst);
    if (front_sleeping.load(std::memory_order_seq_cst)) {
        wake_reasons.fetch_or(reason, std::memory_order_relaxed);
        wake_event.Set();
    }
}
} // namespace Nx2PendingWake

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
        bool latched = false;
        {
            std::scoped_lock lk{mutex};
            for (auto& w : waiters) {
                if (w.address == device_address && !w.latched && Matches(w, value)) {
                    w.latched = true;
                    latched = true;
                }
            }
        }
        if (latched && Nx2PendingWake::On()) {
            Nx2PendingWake::last_latch_ns.store(Nx2PendingWake::NowNs(), std::memory_order_relaxed);
            Nx2PendingWake::Wake(Nx2PendingWake::latch_seq, Nx2PendingWake::ByLatch);
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
