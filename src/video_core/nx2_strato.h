// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "video_core/nx2.h"

// EDEN_NX2_STRATO_STATS=1 (Tegra::Nx2StratoStats): what two ideas from strato would save, logged
// every ~5 s by whichever thread notes something after the window ends.
// - Waits for GPU-written pages (command lists, MME DMA reads): how many and how long, and how many
//   of them began when the last GPU write recorded to the pages was already done on the GPU (a
//   generation check, Tegra::Nx2LastGpuWriteTick, would not have waited for the write).
// - Draw setup: time in GraphicsPipeline::ConfigureImpl against the CPU time of its thread.
namespace VideoCommon::Nx2Strato {

enum class Wait : u32 {
    ListCopy,     ///< A GPU-written command list's copy (BufferCache::Nx2ReadListCopy)
    ListFlush,    ///< A GPU-written command list's pages downloaded (FlushGpuWrittenCommands)
    DmaFlush,     ///< An MME DMA read's GPU-written words downloaded (macro.cpp, FrontDmaRead)
    SplitWriters, ///< The split's front waits for its back to run queued compute and copy work
    Count,
};

struct WaitTotals {
    std::atomic<u64> count{};
    std::atomic<u64> ns{};
    std::atomic<u64> done_count{}; ///< Of count: the pages' last GPU write was already done
    std::atomic<u64> done_ns{};
    std::atomic<u64> gen_skips{};  ///< Waits EDEN_NX2_GPU_DIRTY_GEN did not make
};
inline std::array<WaitTotals, static_cast<size_t>(Wait::Count)> g_waits{};
/// Of the ListCopy waits: the words read were those the page's previous read got.
inline std::atomic<u64> g_same_words_count{};
inline std::atomic<u64> g_same_words_ns{};
/// Direct downloads EDEN_NX2_GPU_DIRTY_GEN let wait for their pages' last write instead of their
/// buffer's, and of them those that found it done while the buffer's was not (no wait at all).
inline std::atomic<u64> g_gen_downloads{};
inline std::atomic<u64> g_gen_downloads_free{};
inline std::atomic<u64> g_configure_ns{};
inline std::atomic<u64> g_configure_calls{};

/// Logs the totals when the window has ended (nx2_strato.cpp).
void MaybeReport();

/// A wait of `ns` for GPU-written pages. `done`: the last GPU write recorded to them was complete
/// when it began (for SplitWriters: once the back had caught up).
inline void NoteWait(Wait kind, u64 ns, bool done) {
    WaitTotals& totals = g_waits[static_cast<size_t>(kind)];
    totals.count.fetch_add(1, std::memory_order_relaxed);
    totals.ns.fetch_add(ns, std::memory_order_relaxed);
    if (done) {
        totals.done_count.fetch_add(1, std::memory_order_relaxed);
        totals.done_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    MaybeReport();
}

/// EDEN_NX2_GPU_DIRTY_GEN read GPU-written pages without the wait the sticky bits make.
inline void NoteGenSkip(Wait kind) {
    g_waits[static_cast<size_t>(kind)].gen_skips.fetch_add(1, std::memory_order_relaxed);
}

/// A direct download waits for its pages' last write, older than its buffer's (`free`: done).
inline void NoteGenDownload(bool free) {
    g_gen_downloads.fetch_add(1, std::memory_order_relaxed);
    if (free) {
        g_gen_downloads_free.fetch_add(1, std::memory_order_relaxed);
    }
}

/// A ListCopy wait of `ns` read the same words as the page's read before.
inline void NoteSameWords(u64 ns) {
    g_same_words_count.fetch_add(1, std::memory_order_relaxed);
    g_same_words_ns.fetch_add(ns, std::memory_order_relaxed);
}

/// Whether the GPU is done with the last write recorded to the pages behind [gpu_address, +size)
/// of `memory` (a Tegra::MemoryManager), asking `rasterizer` (Nx2TickComplete). Pages with no
/// write recorded don't count; false when none has one.
template <typename Memory, typename Rasterizer>
[[nodiscard]] bool WriteDone(Memory& memory, Rasterizer& rasterizer, u64 gpu_address, u64 size) {
    u64 newest = 0;
    for (u64 page = gpu_address & ~u64{0xfff}; page < gpu_address + size; page += 0x1000) {
        if (const auto device = memory.GpuToCpuAddress(page)) {
            newest = (std::max)(newest, Tegra::Nx2LastGpuWriteTick(*device, 1));
        }
    }
    return newest != 0 && rasterizer.Nx2TickComplete(newest);
}

/// A draw spent `ns` in GraphicsPipeline::Configure; the thread's CPU time is sampled now and then.
void NoteConfigure(u64 ns);

/// Nanoseconds, for timing the waits (the wall clock the GPU profile uses).
u64 Now();

} // namespace VideoCommon::Nx2Strato
