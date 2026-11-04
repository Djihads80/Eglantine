// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

// EDEN_NX2_CMDPAGE_DIAG: for command list pages that GPU work writes (nx2.h), whether the work
// writes the lists' own words or only other bytes of their 4 KiB pages, and what the front's slow
// path (DmaPusher::FlushGpuWrittenCommands: drain the split, flush the pages) costs. Writes are
// noted at the ranges the buffer cache marks written (a draw's whole storage binding). Lists on
// draw-written pages that read guest memory without a drain are checked on the back at their
// place in the split: a write sent before them that they missed is counted. Every 2 s the GPU
// thread logs the counts.
namespace Tegra::Nx2CmdPageDiag {

inline const bool g_enabled = std::getenv("EDEN_NX2_CMDPAGE_DIAG") != nullptr;

inline constexpr u64 PageBytes = u64{1} << Nx2WrittenPageBits;
inline constexpr u32 PageWords = static_cast<u32>(PageBytes / sizeof(u32));

/// What wrote a slow-path entry's pages since the front last flushed them; later ones win.
enum class Kind : u32 {
    NoWrite,        ///< Nothing: the drain and flush found nothing new
    OtherElsewhere, ///< Dispatches or copies wrote other words of the pages
    DrawElsewhere,  ///< Draws wrote other words of the pages (false sharing)
    OtherOnEntry,   ///< Dispatches or copies wrote the entry's words
    DrawOnEntry,    ///< Draws wrote the entry's words
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> kind_names{
    "no write", "dispatch/copy elsewhere", "draw elsewhere", "dispatch/copy on entry",
    "draw on entry",
};

enum class Path : u32 { Submitted, Safe, Copy, Slow, Count };

struct Page {
    std::bitset<PageWords> draw_words;  ///< Written outside dispatches and copies since flushed
    std::bitset<PageWords> other_words; ///< Written by dispatches and copies since flushed
    bool ever_draw{};
};

/// One slow-path entry's view of one of its pages.
struct PageView {
    Kind kind{Kind::NoWrite};
    u32 draw_first{PageWords}; ///< Draw-written words of the page, [first, last]
    u32 draw_last{};
};

struct State {
    std::mutex mutex;
    std::unordered_map<u64, Page> pages;
    u64 draw_pages{};

    std::array<std::atomic<u64>, static_cast<size_t>(Path::Count)> entries{};
    std::atomic<u64> slow_drained{};
    std::atomic<u64> slow_flushed{};
    std::atomic<u64> slow_ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> kinds{};
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> kinds_changed{};
    std::atomic<u64> changed_words{};
    std::atomic<u64> reads_copied{};
    std::atomic<u64> reads_synced{};
    std::atomic<u64> reads_unknown{};
    std::atomic<u64> reads_unknown_draw{};
    std::atomic<u64> draw_writes{};
    std::atomic<u64> draw_writes_whole_page{};
    std::atomic<u64> other_writes{};
    std::atomic<u64> sync_waits{};
    std::atomic<u64> sync_wait_ns{};
    std::atomic<u64> missed_on_entry{};
    std::atomic<u64> missed_elsewhere{};
    std::atomic<u32> missed_logged{};
    std::atomic<u64> dma_reads{};
    std::atomic<u64> dma_reads_gpu_pages{};
    std::atomic<u64> dma_reads_changed{};
    std::atomic<u64> dma_reads_changed_gpu_pages{};
    std::atomic<u32> dma_changed_logged{};
    std::array<u32, static_cast<size_t>(Kind::Count)> details_logged{};
    std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
};

inline State& Get() {
    static State state;
    return state;
}

inline u64 Take(std::atomic<u64>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
}

/// The back: GPU work wrote [device_addr, device_addr + size) over the list page at page_addr.
inline void NoteWrite(u64 page_addr, u64 device_addr, u64 size, bool draw) {
    State& state = Get();
    const u64 begin = (std::max)(device_addr, page_addr) - page_addr;
    const u64 end = (std::min)(device_addr + size, page_addr + PageBytes) - page_addr;
    if (draw) {
        state.draw_writes.fetch_add(1, std::memory_order_relaxed);
        if (begin == 0 && end == PageBytes) {
            state.draw_writes_whole_page.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        state.other_writes.fetch_add(1, std::memory_order_relaxed);
    }
    std::scoped_lock lock{state.mutex};
    Page& page = state.pages[page_addr];
    auto& words = draw ? page.draw_words : page.other_words;
    for (u64 word = begin / sizeof(u32); word < (end + sizeof(u32) - 1) / sizeof(u32); ++word) {
        words.set(word);
    }
    if (draw && !page.ever_draw) {
        page.ever_draw = true;
        ++state.draw_pages;
    }
}

/// The front, with the split drained: what wrote the page since it last flushed it, and over
/// the entry's bytes [begin, end) of it. Forgets those writes (the caller flushes the page).
inline PageView TakePage(u64 page_addr, u32 begin, u32 end) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto it = state.pages.find(page_addr);
    if (it == state.pages.end()) {
        return {};
    }
    Page& page = it->second;
    std::bitset<PageWords> entry;
    for (u32 word = begin / sizeof(u32); word < (end + sizeof(u32) - 1) / sizeof(u32); ++word) {
        entry.set(word);
    }
    PageView view;
    if ((page.draw_words & entry).any()) {
        view.kind = Kind::DrawOnEntry;
    } else if ((page.other_words & entry).any()) {
        view.kind = Kind::OtherOnEntry;
    } else if (page.draw_words.any()) {
        view.kind = Kind::DrawElsewhere;
    } else if (page.other_words.any()) {
        view.kind = Kind::OtherElsewhere;
    }
    for (u32 word = 0; word < PageWords; ++word) {
        if (page.draw_words.test(word)) {
            view.draw_first = (std::min)(view.draw_first, word);
            view.draw_last = word;
        }
    }
    page.draw_words.reset();
    page.other_words.reset();
    return view;
}

/// The back, at the place of a list the front read from the guest memory of a draw-written page
/// without draining: whether work sent before the list wrote the page after the front read it.
inline void CheckMissedWrite(u64 page_addr, u32 begin, u32 end) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto it = state.pages.find(page_addr);
    if (it == state.pages.end() || it->second.draw_words.none()) {
        return;
    }
    std::bitset<PageWords> entry;
    for (u32 word = begin / sizeof(u32); word < (end + sizeof(u32) - 1) / sizeof(u32); ++word) {
        entry.set(word);
    }
    if ((it->second.draw_words & entry).none()) {
        state.missed_elsewhere.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.missed_on_entry.fetch_add(1, std::memory_order_relaxed);
    if (state.missed_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_WARNING(HW_GPU,
                    "cmdpage diag: list at page {:#x} bytes {:#x}-{:#x} read without a drain, but "
                    "a draw sent before it wrote those bytes",
                    page_addr, begin, end);
    }
}

/// The front's macro read `words` at `source` after draining (MME DMA): `changed` of them differ
/// from guest memory read before the drain; `gpu_written`: on pages GPU work has written.
inline void RecordDmaRead(u64 source, u32 words, u32 changed, bool gpu_written) {
    State& state = Get();
    state.dma_reads.fetch_add(1, std::memory_order_relaxed);
    state.dma_reads_gpu_pages.fetch_add(gpu_written ? 1 : 0, std::memory_order_relaxed);
    if (changed == 0) {
        return;
    }
    state.dma_reads_changed.fetch_add(1, std::memory_order_relaxed);
    state.dma_reads_changed_gpu_pages.fetch_add(gpu_written ? 1 : 0, std::memory_order_relaxed);
    if (state.dma_changed_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_INFO(HW_GPU,
                 "cmdpage diag: macro DMA read of {} words at {:#x} ({}GPU-written pages): {} "
                 "words changed by the drain",
                 words, source, gpu_written ? "" : "no ", changed);
    }
}

inline void NoteSyncWait(std::chrono::steady_clock::duration waited) {
    State& state = Get();
    state.sync_waits.fetch_add(1, std::memory_order_relaxed);
    state.sync_wait_ns.fetch_add(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()),
        std::memory_order_relaxed);
}

inline bool IsDrawPage(u64 page_addr) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto it = state.pages.find(page_addr);
    return it != state.pages.end() && it->second.ever_draw;
}

inline void Log() {
    State& state = Get();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - state.last_log).count();
    if (seconds < 2.0) {
        return;
    }
    state.last_log = now;
    const auto path = [&](Path p) { return Take(state.entries[static_cast<size_t>(p)]); };
    std::array<u64, static_cast<size_t>(Kind::Count)> kinds{};
    std::array<u64, static_cast<size_t>(Kind::Count)> changed{};
    for (size_t i = 0; i < kinds.size(); ++i) {
        kinds[i] = Take(state.kinds[i]);
        changed[i] = Take(state.kinds_changed[i]);
    }
    u64 draw_pages{};
    {
        std::scoped_lock lock{state.mutex};
        draw_pages = state.draw_pages;
    }
    LOG_INFO(HW_GPU,
             "cmdpage diag {:.1f}s: entries submitted {} safe {} copy {} slow {} | slow drained "
             "{} flushed {} in {:.1f} ms | slow by writer (entries/changed): {} {}/{}, {} {}/{}, "
             "{} {}/{}, {} {}/{}, {} {}/{}; changed words {} | list page reads copied {} synced "
             "{} unknown {} (on draw pages {}) | sync waits {} in {:.1f} ms | missed draw writes "
             "on entry {} elsewhere {} | list page writes draw {} (whole page {}) dispatch/copy "
             "{}; draw-written list pages {} | macro DMA reads {} (on GPU-written pages {}), "
             "changed by the drain {} (on GPU-written pages {})",
             seconds, path(Path::Submitted), path(Path::Safe), path(Path::Copy), path(Path::Slow),
             Take(state.slow_drained), Take(state.slow_flushed),
             static_cast<double>(Take(state.slow_ns)) / 1e6, kind_names[0], kinds[0], changed[0],
             kind_names[1], kinds[1], changed[1], kind_names[2], kinds[2], changed[2],
             kind_names[3], kinds[3], changed[3], kind_names[4], kinds[4], changed[4],
             Take(state.changed_words), Take(state.reads_copied), Take(state.reads_synced),
             Take(state.reads_unknown), Take(state.reads_unknown_draw), Take(state.sync_waits),
             static_cast<double>(Take(state.sync_wait_ns)) / 1e6, Take(state.missed_on_entry),
             Take(state.missed_elsewhere), Take(state.draw_writes),
             Take(state.draw_writes_whole_page), Take(state.other_writes), draw_pages,
             Take(state.dma_reads), Take(state.dma_reads_gpu_pages), Take(state.dma_reads_changed),
             Take(state.dma_reads_changed_gpu_pages));
}

/// The front parsed an entry through `path`.
inline void CountEntry(Path path) {
    if (!g_enabled) {
        return;
    }
    Get().entries[static_cast<size_t>(path)].fetch_add(1, std::memory_order_relaxed);
    Log();
}

/// The front drained and flushed a slow-path entry; `first` is its first page's view.
inline void RecordSlow(Kind kind, u64 changed_words, bool flushed, u64 ns, u64 address, u64 page,
                       u32 begin, u32 end, const PageView& first) {
    State& state = Get();
    const size_t index = static_cast<size_t>(kind);
    state.slow_drained.fetch_add(1, std::memory_order_relaxed);
    state.slow_flushed.fetch_add(flushed ? 1 : 0, std::memory_order_relaxed);
    state.slow_ns.fetch_add(ns, std::memory_order_relaxed);
    state.kinds[index].fetch_add(1, std::memory_order_relaxed);
    state.kinds_changed[index].fetch_add(changed_words != 0 ? 1 : 0, std::memory_order_relaxed);
    state.changed_words.fetch_add(changed_words, std::memory_order_relaxed);
    if (state.details_logged[index] >= 12) {
        return;
    }
    ++state.details_logged[index];
    LOG_INFO(HW_GPU,
             "cmdpage diag: {} at {:#x} (page {:#x} bytes {:#x}-{:#x}): {} words changed by the "
             "flush; draw-written bytes of the page {}",
             kind_names[index], address, page, begin, end, changed_words,
             first.draw_first <= first.draw_last
                 ? fmt::format("{:#x}-{:#x}", first.draw_first * 4, first.draw_last * 4 + 4)
                 : std::string{"none"});
}

} // namespace Tegra::Nx2CmdPageDiag
