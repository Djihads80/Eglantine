// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>

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
    SignalOrdering, ///< SignalOrdering: mostly waiting for the buffer cache lock
    ListCopyWait,   ///< Nx2ReadListCopy: the host GPU finishing a command list copy
    BindUniformBank,   ///< Of bind stage buffers: native 64 KiB bank page diffing
    BindUniformStream, ///< Of bind stage buffers: uniforms streamed (small or misaligned)
    BindUniformCached, ///< Of bind stage buffers: cached uniforms (touch, sync, usage, bind)
    BindStorage,       ///< Of bind stage buffers: storage buffers (touch, sync, usage, bind)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "signal ordering",
    "list copy wait",
    "bind uniform bank", "bind uniform stream", "bind uniform cached", "bind storage",
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

/// Why the Vulkan scheduler submitted (Scheduler::Flush): the caller tags it with FlushOriginTag.
enum class FlushOrigin : u32 {
    Other,
    Waited,   ///< Nx2Perf submit_while_waited: the front waited for the back
    GpuIdle,  ///< Nx2Perf submit_while_waited: the host GPU had finished everything submitted
    Budget,   ///< FlushWork's draw budget
    Fence,    ///< FlushCommands (fences with downloads or queries), not deferred
    ListCopy, ///< Command list copies (SubmitNx2ListCopies, DMA copies)
    IdleHook, ///< The split back's idle hook submitting deferred work
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(FlushOrigin::Count)>
    flush_origin_names{"other", "waited", "gpu idle", "budget", "fence", "list copy", "idle hook"};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 32> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(FlushOrigin::Count)> flushes{};
    u64 list_copy_done_ns{}; ///< Command list copies the front waited for: recorded to done
    u64 list_copy_done_count{};
    /// Executable methods the front sent with the value the register held (approximate top 16,
    /// space-saving: a new method takes the least counted slot and inherits its count).
    std::array<std::pair<u32, u64>, 16> same_executable{};
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
    AcquirePolls,        ///< Pending semaphore acquires polled (Puller::CanResumeAcquire)
    AcquirePollsMet,     ///< Of those, met (the channel resumes)
    AcquirePollsEarly,   ///< Of AcquirePollsMet: without draining the back (Opt::AcquireEarly)
    AcquireTriggerEarly, ///< Of AcquireChecks: met without draining the back (Opt::AcquireEarly)
    DirectWrites,        ///< 3D register writes the split front sent from command lists
    MacroWrites,         ///< 3D register writes the split front's macros sent
    SameExecutableWrites, ///< Executable writes the front sent with the register's value
    /// Of DownloadBytes, by the age in frames of their 64 KiB block's preflush stamp
    /// (BufferCache::nx2_preflush_blocks): < 60, < 120, < 600, < 3600, older, no stamp
    DownloadAge60,
    DownloadAge120,
    DownloadAge600,
    DownloadAge3600,
    DownloadAgeOlder,
    DownloadAgeNone,
    BindMemoHits,        ///< Nx2Perf bind_memo: binds that skipped touch, sync and usage
    BindMemoMisses,      ///< Nx2Perf bind_memo: binds that took the full path
    DmaReadsWriterSet,   ///< Of DmaReadsEarly: read with proven draws still queued (MacroReadWriterSet)
    DmaReadsWriterSetFallback, ///< Drained instead: a queued proven draw's proof no longer held
};

/// The fencing thread's buffer downloads (BufferCache::PopAsyncBuffers): logged by the thread
/// that makes the fences.
inline std::atomic<u64> g_fencing_lock_ns{};       ///< Holding the buffer cache lock
inline std::atomic<u64> g_fencing_written_bytes{}; ///< Written back to guest memory
inline std::atomic<u64> g_fencing_pops{};          ///< Fences popped

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

/// An executable 3D method the front sent with the value the register held (same_executable).
inline void AddSameExecutable(u32 method) {
    if (!Enabled()) {
        return;
    }
    auto& slots = t_data.same_executable;
    ++t_data.counters[static_cast<size_t>(Counter::SameExecutableWrites)];
    size_t least = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].second != 0 && slots[i].first == method) {
            ++slots[i].second;
            return;
        }
        if (slots[i].second < slots[least].second) {
            least = i;
        }
    }
    slots[least].first = method;
    ++slots[least].second;
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

inline thread_local FlushOrigin t_flush_origin{FlushOrigin::Other};

/// Tags the scheduler submissions made in its scope (only while profiling); nests.
class FlushOriginTag {
public:
    explicit FlushOriginTag(FlushOrigin origin) : enabled{Enabled()} {
        if (enabled) {
            previous = t_flush_origin;
            t_flush_origin = origin;
        }
    }
    ~FlushOriginTag() {
        if (enabled) {
            t_flush_origin = previous;
        }
    }

    FlushOriginTag(const FlushOriginTag&) = delete;
    FlushOriginTag& operator=(const FlushOriginTag&) = delete;

private:
    bool enabled;
    FlushOrigin previous{};
};

/// Scheduler::Flush: one submission, counted by the current tag.
inline void AddFlush() {
    if (Enabled()) {
        ++t_data.flushes[static_cast<size_t>(t_flush_origin)];
    }
}

/// The front waited for a command list copy recorded at `recorded` (Now; 0 when not profiling)
/// and it is done now.
inline void AddListCopyDone(u64 recorded) {
    if (Enabled() && recorded != 0) {
        t_data.list_copy_done_ns += Now() - recorded;
        ++t_data.list_copy_done_count;
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
    const auto per_second = [&](Counter counter) {
        return static_cast<double>(data.counters[static_cast<size_t>(counter)]) * 1e9 / window;
    };
    line += fmt::format(" | acquire polls {:.0f} ({:.0f} met, {:.0f} without a drain) | acquires "
                        "met without a drain {:.0f}",
                        per_second(Counter::AcquirePolls), per_second(Counter::AcquirePollsMet),
                        per_second(Counter::AcquirePollsEarly),
                        per_second(Counter::AcquireTriggerEarly));
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
    if (data.counters[static_cast<size_t>(Counter::DirectWrites)] != 0 || data.counters[static_cast<size_t>(Counter::MacroWrites)] != 0) {
        line += fmt::format(" | front 3D writes: direct {:.0f}, from macros {:.0f}",
                            static_cast<double>(data.counters[static_cast<size_t>(Counter::DirectWrites)]) * 1e9 / window,
                            static_cast<double>(data.counters[static_cast<size_t>(Counter::MacroWrites)]) * 1e9 / window);
    }
    if (data.counters[static_cast<size_t>(Counter::SameExecutableWrites)] != 0) {
        auto slots = data.same_executable;
        std::sort(slots.begin(), slots.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        line += fmt::format(" | same-value executable writes sent {:.0f}, top:",
                            static_cast<double>(data.counters[static_cast<size_t>(Counter::SameExecutableWrites)]) * 1e9 / window);
        for (const auto& [method, count] : slots) {
            if (count != 0) {
                line += fmt::format(" {:#x} {:.0f},", method,
                                    static_cast<double>(count) * 1e9 / window);
            }
        }
        line.pop_back();
    }
    const auto per_second_kib = [&](size_t counter) {
        return static_cast<double>(data.counters[counter]) * 1e9 / 1024.0 / window;
    };
    line += fmt::format(" | downloads by preflush stamp age (KiB): <60f {:.0f}, <120f {:.0f}, "
                        "<600f {:.0f}, <3600f {:.0f}, older {:.0f}, none {:.0f}",
                        per_second_kib(static_cast<size_t>(Counter::DownloadAge60)), per_second_kib(static_cast<size_t>(Counter::DownloadAge120)), per_second_kib(static_cast<size_t>(Counter::DownloadAge600)),
                        per_second_kib(static_cast<size_t>(Counter::DownloadAge3600)), per_second_kib(static_cast<size_t>(Counter::DownloadAgeOlder)), per_second_kib(static_cast<size_t>(Counter::DownloadAgeNone)));
    if (data.count[static_cast<size_t>(Zone::Fence)] != 0) {
        const u64 lock_ns = g_fencing_lock_ns.exchange(0, std::memory_order_relaxed);
        const u64 written = g_fencing_written_bytes.exchange(0, std::memory_order_relaxed);
        const u64 pops = g_fencing_pops.exchange(0, std::memory_order_relaxed);
        line += fmt::format(" | fencing thread: buffer cache held {:.1f} ms for {:.0f} pops, "
                            "{:.0f} KiB written back",
                            static_cast<double>(lock_ns) * 1000.0 / window,
                            static_cast<double>(pops) * 1e9 / window,
                            static_cast<double>(written) * 1e9 / 1024.0 / window);
    }
    if (data.counters[static_cast<size_t>(Counter::BindMemoHits)] != 0 || data.counters[static_cast<size_t>(Counter::BindMemoMisses)] != 0) {
        line += fmt::format(" | bind memo {:.0f} hits, {:.0f} misses",
                            static_cast<double>(data.counters[static_cast<size_t>(Counter::BindMemoHits)]) * 1e9 / window,
                            static_cast<double>(data.counters[static_cast<size_t>(Counter::BindMemoMisses)]) * 1e9 / window);
    }
    line += fmt::format(" | MME DMA reads past proven draws {:.0f} ({:.0f} drained: proof changed)",
                        static_cast<double>(data.counters[static_cast<size_t>(Counter::DmaReadsWriterSet)]) * 1e9 / window,
                        static_cast<double>(data.counters[static_cast<size_t>(Counter::DmaReadsWriterSetFallback)]) * 1e9 / window);
    std::string flushes;
    for (size_t i = 0; i < data.flushes.size(); ++i) {
        if (data.flushes[i] != 0) {
            flushes += fmt::format(" {} {:.0f},", flush_origin_names[i],
                                   static_cast<double>(data.flushes[i]) * 1e9 / window);
        }
    }
    if (!flushes.empty()) {
        flushes.pop_back();
        line += " | submits by origin:" + flushes;
    }
    if (data.list_copy_done_count != 0) {
        line += fmt::format(" | list copy: {:.0f} us from submit to done ({:.0f}/s)",
                            static_cast<double>(data.list_copy_done_ns) / 1000.0 /
                                static_cast<double>(data.list_copy_done_count),
                            static_cast<double>(data.list_copy_done_count) * 1e9 / window);
    }
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
