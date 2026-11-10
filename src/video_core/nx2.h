// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/settings.h"

namespace Tegra {

/// EDEN_NX2_LIST_PREDICT=<n>|a (DmaPusher::PredictGpuWrittenCommands): the unchanged reads a
/// GPU-written command list needs before it is run with the words of its read before ("a": 0);
/// -1: off. Read on first use, after env.txt.
[[nodiscard]] inline int Nx2ListPredict() {
    static const int streak = [] {
        const char* const value = std::getenv("EDEN_NX2_LIST_PREDICT");
        if (value != nullptr && value[0] == 'a') {
            return 0;
        }
        if (value != nullptr && value[0] >= '1' && value[0] <= '9') {
            return std::atoi(value);
        }
        return -1;
    }();
    return streak;
}

/// With list prediction (and channels that suspend on a pending copy, EDEN_NX2_LIST_SUSPEND),
/// the copies of GPU-written command list pages are not submitted right after their writer: a
/// predicted list doesn't wait for its copy, and a list that does wait asks for the submission
/// (RasterizerInterface::IsGpuTickDone). Ocarina of Time made ~124 submissions a frame.
/// EDEN_NX2_LIST_LAZY_SUBMIT=0: submit at once as before.
[[nodiscard]] inline bool Nx2ListLazySubmit() {
    static const bool lazy = [] {
        const char* const off = std::getenv("EDEN_NX2_LIST_LAZY_SUBMIT");
        const char* const suspend = std::getenv("EDEN_NX2_LIST_SUSPEND");
        return Nx2ListPredict() >= 0 && (off == nullptr || off[0] != '0') &&
               (suspend == nullptr || suspend[0] != '0');
    }();
    return lazy;
}

/// EDEN_NX2_LIST_DIFF: macro calls left to trace (their parameters and method writes are logged):
/// set when a GPU-written command list is read, to see what its words are for.
inline std::atomic<int> g_nx2_trace_macros{0};

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// The running title, for the few places that must behave differently per game.
inline std::atomic<u64> g_nx2_program_id{0};

// EDEN_NX2_HELD_WRITES=0 turns off, for Switch 2 programs: applying the guest CPU writes held
// back from GPU-written memory before each submitted command list, and applying them byte for
// byte (OnCachedWrite) instead of re-uploading whole pages over GPU data never copied back.
inline bool Nx2HeldWritesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_HELD_WRITES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

inline bool Nx2HeldWrites() {
    return g_nx2_ampere.load(std::memory_order_relaxed) && Nx2HeldWritesEnabled();
}

// nx2_command_lists, set once per game start (nvdrv's Module; EDEN_NX2_COMMAND_LISTS=0/1
// overrides it). Off, the command list series is: no copy of a list's words at submit
// (nvhost_gpu), and no copies or flushes of lists on GPU-written pages (DmaPusher's
// IsGpuWrittenCommands, ReadGpuWrittenCommands). Lists are then read from guest memory when they
// run, as before the series.
inline std::atomic<bool> g_nx2_command_lists{true};

inline bool Nx2CommandLists() {
    return g_nx2_command_lists.load(std::memory_order_relaxed);
}

// Set (Core::Memory, as the application's page table is set up) while the guest CPU's reads of
// GPU-written pages are trapped: read traps, under Dynarmic or NCE (EDEN_NX2_READ_TRAPS,
// EDEN_NX2_NCE_READ_TRAPS). A trapped read downloads what it reads and has later GPU writes there
// copied back at fences.
inline std::atomic<bool> g_nx2_cpu_read_traps{false};

// Titles whose guest side reads GPU-written buffer data through a path the buffer cache does not
// see, so their buffers must be copied back to guest memory at every fence
// (BufferCache::AsyncBufferDownloads, EDEN_NX2_BUFFER_DOWNLOADS). 0400A3A01EE4C000 is Pokemon
// Pokopia: with the copy-back off its sky is black or flickers. 0400486024DEC000 is Ocarina of
// Time: its CPU reads GPU-written exposure/sky values, and without read traps (Android before NCE
// had them: reactive flushing is off there) it read stale ones, so the scene came out ~3.4x
// overexposed (white mist from the sky LUT on). Its reads are guest CPU reads, which read traps
// catch, so it is listed only while they are off.
inline bool Nx2NeedsBufferDownloads() {
    switch (g_nx2_program_id.load(std::memory_order_relaxed)) {
    case 0x0400A3A01EE4C000ULL:
        return true;
    case 0x0400486024DEC000ULL:
        return !g_nx2_cpu_read_traps.load(std::memory_order_relaxed);
    default:
        return false;
    }
}

// Per-title defaults of a few switches. An explicit environment variable always wins; each is
// read the first time it is needed, never at namespace scope (Android applies env.txt in
// Core::System::Load, after the library's statics). The title is the program the core loaded
// (Settings::GetCurrentProgramID, set in Load before the GPU, its thread and the disk shader
// cache exist), not g_nx2_program_id, which is set later, at the game's first GPU
// characteristics query (the disk cache's shaders are translated before that).
inline constexpr u64 Nx2MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 Nx2OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;

[[nodiscard]] inline u64 Nx2DefaultsProgramId() {
    return Settings::GetCurrentProgramID();
}

/// The GPU thread split's mode (GpuSplit::MaybeStart): '0' off, '1' engine methods on a second
/// thread, '2' that with the macros left on the first. EDEN_NX2_SPLIT=<mode> overrides the
/// title's default: '2' for Mario Kart World (Odin 3: ~42 fps vs ~38 off) and for titles not
/// listed; on Android '0' for Ocarina of Time (Odin 3: '2' is slower and stutters, 16-17 fps vs
/// ~20 off; '1' gains nothing). The PC keeps '2' for it (~44-56 fps in Kokiri Forest).
[[nodiscard]] inline char Nx2SplitMode() {
    static const int forced = [] {
        const char* const value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? int{value[0]} : -1;
    }();
    if (forced >= 0) {
        return static_cast<char>(forced);
    }
    switch (Nx2DefaultsProgramId()) {
    case Nx2MarioKartWorldProgramId:
        return '2';
#ifdef __ANDROID__
    case Nx2OcarinaOfTimeProgramId:
        return '0';
#endif
    default:
        return '2';
    }
}

/// Whether the rasterizer skips telling its caches about the GPU thread's own writes to guest
/// memory (MemoryManager::FlushCaching -> InnerInvalidation), as the skip_cpu_inner_invalidation
/// setting does for every title. EDEN_NX2_SKIP_INNER_INVALIDATION=0/1 overrides the title's
/// default: on for Mario Kart World, where it took 1.9 of the 23 ms of a frame on the GPU back
/// thread (Odin 3: ~+2 fps, the picture unchanged in the race).
[[nodiscard]] inline bool Nx2SkipInnerInvalidation() {
    static const int forced = [] {
        const char* const value = std::getenv("EDEN_NX2_SKIP_INNER_INVALIDATION");
        return value != nullptr ? int{value[0] != '0'} : -1;
    }();
    if (forced >= 0) {
        return forced != 0;
    }
    return Nx2DefaultsProgramId() == Nx2MarioKartWorldProgramId;
}

/// How long the GPU thread sleeps between polls of channels suspended on a semaphore the CPU
/// releases (gpu_thread.cpp, #446), in microseconds. EDEN_NX2_PENDING_SLEEP_US=<us> (> 0)
/// overrides the title's default: 50 us for Ocarina of Time, which waits there ~330 times a
/// second while its present waits for the work behind them (~17 ms a frame in Kokiri village
/// with 1 ms sleeps, ~1 ms with 50 us); 1 ms elsewhere (50 us cost Mario Kart World ~1 fps on
/// the Odin 3).
[[nodiscard]] inline long Nx2PendingSleepUs() {
    static const long forced = [] {
        const char* const value = std::getenv("EDEN_NX2_PENDING_SLEEP_US");
        return value != nullptr ? std::strtol(value, nullptr, 10) : 0L;
    }();
    if (forced > 0) {
        return forced;
    }
    return Nx2DefaultsProgramId() == Nx2OcarinaOfTimeProgramId ? 50L : 1000L;
}

/// Whether compute programs may bind up to 64 storage buffers (Shader::Info::MAX_SSBOS) instead
/// of 32 (global_memory_to_storage_buffer_pass.cpp, de4ed9: Ocarina of Time's Hyrule Field
/// grass lists). EDEN_NX2_WIDE_COMPUTE_SSBOS=0/1 overrides the title's default: on, except for
/// Ocarina of Time on Android, where it brings back the big white leaf flashes at the Kokiri
/// ladder (Odin 3: 3-5 in 60 s vs 0).
[[nodiscard]] inline bool Nx2WideComputeSsbos() {
    static const int forced = [] {
        const char* const value = std::getenv("EDEN_NX2_WIDE_COMPUTE_SSBOS");
        return value != nullptr && value[0] != '\0' ? int{value[0] != '0'} : -1;
    }();
    if (forced >= 0) {
        return forced != 0;
    }
#ifdef __ANDROID__
    return Nx2DefaultsProgramId() != Nx2OcarinaOfTimeProgramId;
#else
    return true;
#endif
}

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list (a bit cleared once the page's last write completed would let the front
// read it without waiting for that dispatch). EDEN_NX2_GPU_DIRTY_GEN keeps them and shortens the
// waits behind them instead. One bit per 4 KiB of the 36-bit device address space (2 MiB).
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    // A word at a time: storage buffer binds mark megabytes, mostly marked already.
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

/// EDEN_NX2_STRATO_STATS=1: a line every ~5 s on the waits GPU-written pages cause (whether the
/// last GPU write to the pages was already done as each began) and on the time draws spend in
/// GraphicsPipeline::ConfigureImpl (nx2_strato.h).
[[nodiscard]] inline bool Nx2StratoStats() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_STRATO_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// EDEN_NX2_GPU_DIRTY_GEN=1: a read of GPU-written pages waits only for the GPU tick of the last
/// write recorded to those pages (Nx2LastGpuWriteTick) instead of the last one to their whole
/// buffer or to their command list copy, and reads at once when the GPU is past it (strato's
/// usage tracker, by generation instead of intervals cleared at waited submits). Off by default.
/// Correct with the split on: which pages count as GPU-written (the sticky bits above) does not
/// change, so its front still waits for queued writers before such a read, and the ticks are
/// looked at only after that wait or a drain, when the back has recorded every write queued
/// before the read. Clearing the bits instead is what the split cannot have (see above).
[[nodiscard]] inline bool Nx2GpuDirtyGen() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_GPU_DIRTY_GEN");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// EDEN_NX2_GUEST_FLUSH=1: when the guest CPU stores to a page that holds GPU-written buffer data,
/// the GPU's bytes must reach guest memory first. The GPU thread did that: it stopped recording
/// and waited for the host GPU (Ocarina of Time: ~6 ms of each 43 ms frame, the GPU running dry
/// afterwards). On a device whose buffers the host can read, the faulting guest thread now waits
/// for the tick of the last GPU write to those pages itself and copies the bytes from the
/// buffer's memory under the cache locks; the GPU thread only submits when that write is not
/// submitted yet. Anything else (data the host can't read, a page written again and again) falls
/// back to the GPU thread. EDEN_NX2_GUEST_FLUSH=0/1 overrides the default: on for Android
/// titles that run without the GPU thread split (Ocarina of Time on the Odin 3: 43.5 -> 39.2 ms
/// a frame, 98.7% of ~1,600 such stores a second needing no wait at all), off elsewhere (the
/// split's back owns the caches).
[[nodiscard]] inline bool Nx2GuestFlush() {
    static const int forced = [] {
        const char* const value = std::getenv("EDEN_NX2_GUEST_FLUSH");
        return value != nullptr ? int{value[0] != '0'} : -1;
    }();
    if (forced >= 0) {
        return forced != 0;
    }
#ifdef __ANDROID__
    return Nx2SplitMode() == '0';
#else
    return false;
#endif
}

/// Whether the per-page write ticks below are kept.
[[nodiscard]] inline bool Nx2GpuWriteTicks() {
    return Nx2GpuDirtyGen() || Nx2StratoStats() || Nx2GuestFlush();
}

// The scheduler tick of the last GPU write recorded to each 4 KiB device page (every
// BufferCache::NoteGpuWrite: shader stores, copies, clears, uploads), 0 for none. Two levels:
// 2^24 pages, and a second-level block (32 KiB, 16 MiB of addresses) made on its first write.
inline constexpr u64 Nx2TickBlockBits = 12;
inline std::array<std::atomic<std::atomic<u64>*>,
                  (u64{1} << (36 - Nx2WrittenPageBits - Nx2TickBlockBits))>
    g_nx2_write_tick_blocks{};
/// Bumped when the ticks are cleared (a new session's scheduler counts from 1 again).
inline std::atomic<u32> g_nx2_write_tick_epoch{};

inline void Nx2NoteGpuWriteTick(u64 device_address, u64 size, u64 tick) {
    if (size == 0 || tick == 0) {
        return;
    }
    // The same range at the same tick again (a storage buffer bound for draw after draw): its
    // pages hold that tick already, or a newer one.
    struct Recent {
        u64 address;
        u64 size;
        u64 tick;
        u32 epoch;
    };
    thread_local std::array<Recent, 64> recent{};
    const u32 epoch = g_nx2_write_tick_epoch.load(std::memory_order_relaxed);
    Recent& slot = recent[((device_address >> 8) ^ (device_address >> 20) ^ size) % recent.size()];
    if (slot.address == device_address && slot.size == size && slot.tick == tick &&
        slot.epoch == epoch) {
        return;
    }
    slot = {device_address, size, tick, epoch};
    constexpr u64 block_pages = u64{1} << Nx2TickBlockBits;
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_write_tick_blocks.size() * block_pages - 1);
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        auto& slot_block = g_nx2_write_tick_blocks[page >> Nx2TickBlockBits];
        std::atomic<u64>* block = slot_block.load(std::memory_order_acquire);
        if (block == nullptr) {
            auto* const fresh = new std::atomic<u64>[block_pages]{};
            if (slot_block.compare_exchange_strong(block, fresh, std::memory_order_acq_rel)) {
                block = fresh;
            } else {
                delete[] fresh;
            }
        }
        auto& entry = block[page & (block_pages - 1)];
        u64 current = entry.load(std::memory_order_relaxed);
        while (current < tick &&
               !entry.compare_exchange_weak(current, tick, std::memory_order_relaxed)) {
        }
    }
}

/// The newest tick Nx2NoteGpuWriteTick recorded to the pages of [device_address, +size), 0 if none.
[[nodiscard]] inline u64 Nx2LastGpuWriteTick(u64 device_address, u64 size) {
    if (size == 0) {
        return 0;
    }
    constexpr u64 block_pages = u64{1} << Nx2TickBlockBits;
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_write_tick_blocks.size() * block_pages - 1);
    u64 newest = 0;
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        const std::atomic<u64>* const block =
            g_nx2_write_tick_blocks[page >> Nx2TickBlockBits].load(std::memory_order_acquire);
        if (block == nullptr) {
            page |= block_pages - 1;
            continue;
        }
        newest = std::max<u64>(newest, block[page & (block_pages - 1)].load(std::memory_order_relaxed));
    }
    return newest;
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
    for (auto& slot_block : g_nx2_write_tick_blocks) {
        if (std::atomic<u64>* const block = slot_block.load(std::memory_order_acquire)) {
            for (u64 i = 0; i < (u64{1} << Nx2TickBlockBits); ++i) {
                block[i].store(0, std::memory_order_relaxed);
            }
        }
    }
    g_nx2_write_tick_epoch.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

/// False only when no page of [device_address, device_address + size) has been GPU-written.
/// Every range the buffer cache records as GPU-modified also marks its pages here (under its
/// lock). Ranges past the bitmap, or that wrap, answer true.
[[nodiscard]] inline bool Nx2MayBeGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return false;
    }
    const u64 first = device_address >> Nx2WrittenPageBits;
    const u64 last = (device_address + size - 1) >> Nx2WrittenPageBits;
    if (last < first || last >= g_nx2_gpu_written_pages.size() * 64) {
        return true;
    }
    for (u64 page = first; page <= last; ++page) {
        if ((g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
            return true;
        }
    }
    return false;
}

// Device pages with a query cache write still to land: counter reports and fence-timed releases
// are written when their fence is released, not when the report runs, so an MME DMA read of one
// (occlusion predicates) flushes the query cache over the read first or reads an older value
// (Lichii 9d17457f6c; counting per page after the water agent's query_pending_flush.patch).
// Hashed slots: a collision costs a flush, never a stale read.
inline constexpr size_t Nx2PendingQuerySlots = 4096;
inline std::array<std::atomic<s32>, Nx2PendingQuerySlots> g_nx2_pending_queries{};

[[nodiscard]] inline std::atomic<s32>& Nx2PendingQuerySlot(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    return g_nx2_pending_queries[(page ^ (page >> 12)) % Nx2PendingQuerySlots];
}

// The same counts by 16 bytes (a report writes 4 or 16): Ocarina of Time's macros read 24-byte
// records from pages its reports keep writing, and on the split every such read drained the back
// (~10,000 a second on the phone) though the record read had no write pending. Hashed slots too.
inline constexpr size_t Nx2PendingQueryUnitSlots = 8192;
inline std::array<std::atomic<s32>, Nx2PendingQueryUnitSlots> g_nx2_pending_query_units{};

[[nodiscard]] inline std::atomic<s32>& Nx2PendingQueryUnit(u64 device_address) {
    const u64 unit = device_address >> 4;
    return g_nx2_pending_query_units[(unit * 0x9e3779b97f4a7c15ULL) >> 51];
}

/// EDEN_NX2_QUERY_PENDING_EXACT=0: MME DMA reads look for pending query writes on their whole
/// pages, as before the 16-byte counts.
[[nodiscard]] inline bool Nx2QueryPendingExact() {
    static const bool exact = [] {
        const char* const value = std::getenv("EDEN_NX2_QUERY_PENDING_EXACT");
        return value == nullptr || value[0] != '0';
    }();
    return exact;
}

/// The query cache deferred a write of the word at `device_address` to a fence.
inline void Nx2NotePendingQuery(u64 device_address) {
    Nx2PendingQuerySlot(device_address).fetch_add(1, std::memory_order_release);
    // Up to 16 bytes from the address (a long report: value and timestamp).
    Nx2PendingQueryUnit(device_address).fetch_add(1, std::memory_order_release);
    if (((device_address + 15) >> 4) != (device_address >> 4)) {
        Nx2PendingQueryUnit(device_address + 15).fetch_add(1, std::memory_order_release);
    }
}

/// The deferred write's fence operation ran (whether or not it wrote).
inline void Nx2DonePendingQuery(u64 device_address) {
    Nx2PendingQuerySlot(device_address).fetch_sub(1, std::memory_order_release);
    Nx2PendingQueryUnit(device_address).fetch_sub(1, std::memory_order_release);
    if (((device_address + 15) >> 4) != (device_address >> 4)) {
        Nx2PendingQueryUnit(device_address + 15).fetch_sub(1, std::memory_order_release);
    }
}

/// Whether a deferred query write may still be pending in the device range.
[[nodiscard]] inline bool Nx2HasPendingQuery(u64 device_address, u64 size) {
    if (size == 0) {
        return false;
    }
    for (u64 page = device_address >> Nx2WrittenPageBits;
         page <= (device_address + size - 1) >> Nx2WrittenPageBits; ++page) {
        if (Nx2PendingQuerySlot(page << Nx2WrittenPageBits).load(std::memory_order_acquire) > 0) {
            return true;
        }
    }
    return false;
}

/// As Nx2HasPendingQuery, by 16 bytes for reads up to a page (Nx2QueryPendingExact).
[[nodiscard]] inline bool Nx2ReadHasPendingQuery(u64 device_address, u64 size) {
    if (size == 0) {
        return false;
    }
    if (!Nx2QueryPendingExact() || size > 0x1000) {
        return Nx2HasPendingQuery(device_address, size);
    }
    for (u64 unit = device_address >> 4; unit <= (device_address + size - 1) >> 4; ++unit) {
        if (Nx2PendingQueryUnit(unit << 4).load(std::memory_order_acquire) > 0) {
            return true;
        }
    }
    return false;
}

/// EDEN_NX2_DMA_QUERY_FLUSH=0: MME DMA reads don't flush the query cache over deferred writes.
/// On by default again: Mario Kart World's vertices explode without it, and the Ocarina of Time
/// garbage it was suspected of came from semaphores moved backwards and lost held CPU writes.
[[nodiscard]] inline bool Nx2DmaQueryFlush() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_DMA_QUERY_FLUSH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
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

// GPU-only regions nx2:gpu backs whole (214 without the sparse flag), start -> size: the texture
// cache gives back the guest memory of images it uploaded from them (release_gpu_only_memory).
inline std::mutex g_nx2_gpu_only_mutex;
inline std::map<u64, u64> g_nx2_gpu_only_regions;

inline void Nx2GpuOnlyRegionCreated(u64 gpu_address, u64 size) {
    std::scoped_lock lk{g_nx2_gpu_only_mutex};
    g_nx2_gpu_only_regions[gpu_address] = size;
}

inline void Nx2GpuOnlyRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_gpu_only_mutex};
    g_nx2_gpu_only_regions.erase(gpu_address);
}

/// A new emulation session: the previous game's GPU-only regions are gone too.
inline void Nx2ClearGpuOnlyRegions() {
    std::scoped_lock lk{g_nx2_gpu_only_mutex};
    g_nx2_gpu_only_regions.clear();
}

inline bool Nx2InGpuOnlyRegion(u64 gpu_address, u64 size) {
    std::scoped_lock lk{g_nx2_gpu_only_mutex};
    auto it = g_nx2_gpu_only_regions.upper_bound(gpu_address);
    if (size == 0 || it == g_nx2_gpu_only_regions.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second;
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
