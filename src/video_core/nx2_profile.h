// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <numeric>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
};

// Read once at startup: Enabled() runs several times per method write (no guard to check).
inline const bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

/// One 3D macro start id's ((method - 0xe00) / 2) calls on the split front (FrontRunMacro).
struct MacroCounts {
    u64 calls;
    u64 writes;     ///< Methods the macro emitted
    u64 same_value; ///< Of those, dropped as same-value writes
    u64 ring_words; ///< Ring words sent during the call (writes, DMA read flushes, draws)
    u64 dma_reads;  ///< MME_DMA_READ(_FIFOED) emits
};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 15> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    std::array<MacroCounts, 128> macros{};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Now();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Now() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    DmaReadsEarly,  ///< MME DMA reads without a drain (Maxwell3D::FrontDmaReadEarly)
    DmaReadsWaitUnknown, ///< Of those, waited for a write the front can't place
    DmaReadsWaitGpu,     ///< waited for all queued GPU work with unknown destinations
    DmaReadsWaitWrite,   ///< waited for a noted write to the words' pages
    ListChecks,          ///< Command list fetches waiting only for their writers (ListTarget)
    ListWaits,           ///< Of those, the back still had them queued
    ListWaitsUnknown,    ///< Of ListWaits: for a write the front can't place
};

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

inline u64 CounterValue(Counter counter) {
    return t_data.counters[static_cast<size_t>(counter)];
}

inline void AddMacro(u32 id, u64 writes, u64 same_value, u64 ring_words, u64 dma_reads) {
    MacroCounts& counts = t_data.macros[id % t_data.macros.size()];
    ++counts.calls;
    counts.writes += writes;
    counts.same_value += same_value;
    counts.ring_words += ring_words;
    counts.dma_reads += dma_reads;
}

/// The front waited for the back since `start` (Now) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Now() - start;
        ++t_data.drain_count[index];
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window > zoned ? window - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    line += fmt::format(" | MME DMA reads without a drain {:.0f} (waited for: unknown writes "
                        "{:.0f}, GPU work {:.0f}, writes to the words {:.0f})",
                        static_cast<double>(data.counters[8]) * 1e9 / window,
                        static_cast<double>(data.counters[9]) * 1e9 / window,
                        static_cast<double>(data.counters[10]) * 1e9 / window,
                        static_cast<double>(data.counters[11]) * 1e9 / window);
    line += fmt::format(" | list fetches by their writers {:.0f} ({:.0f} waited, {:.0f} for "
                        "unknown writes)",
                        static_cast<double>(data.counters[12]) * 1e9 / window,
                        static_cast<double>(data.counters[13]) * 1e9 / window,
                        static_cast<double>(data.counters[14]) * 1e9 / window);
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / window,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    std::array<u32, std::tuple_size_v<decltype(data.macros)>> order;
    std::iota(order.begin(), order.end(), 0U);
    std::partial_sort(order.begin(), order.begin() + 12, order.end(), [&data](u32 a, u32 b) {
        return data.macros[a].calls > data.macros[b].calls;
    });
    std::string macros;
    for (size_t i = 0; i < 12 && data.macros[order[i]].calls != 0; ++i) {
        const MacroCounts& counts = data.macros[order[i]];
        const double calls = static_cast<double>(counts.calls);
        macros += fmt::format("\n  macro {:#04x}: {:>9.0f} calls/s, {:5.2f} writes/call ({:5.2f} same "
                              "value), {:5.2f} ring words/call, {:7.0f} DMA reads/s",
                              order[i], calls * 1e9 / window,
                              static_cast<double>(counts.writes) / calls,
                              static_cast<double>(counts.same_value) / calls,
                              static_cast<double>(counts.ring_words) / calls,
                              static_cast<double>(counts.dma_reads) * 1e9 / window);
    }
    if (!macros.empty()) {
        LOG_INFO(HW_GPU, "{} macros per second (top 12 by calls):{}", thread_name, macros);
    }
    data = {};
    data.window_start = now;
}

} // namespace VideoCommon::Nx2Profile
