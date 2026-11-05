// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "common/common_types.h"
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
    SplitWriters, ///< Split GPU thread: the front waits for queued compute/copy work (WaitForWriters)
    SplitDmaRead, ///< Split GPU thread: the front drains the back for a macro DMA read
    UpdateIndex,  ///< Of UpdateBuffers: the index buffer
    UpdateVertex, ///< Of UpdateBuffers: the vertex buffers
    UpdateStages, ///< Of UpdateBuffers: uniform, storage and texture buffers of every stage
    UboReuse,     ///< Of BindStageBuffers: small uniform buffers compared with their last stream copy
    UboCopy,      ///< Of BindStageBuffers: small uniform buffers copied into the stream buffer
    UboCached,    ///< Of BindStageBuffers: uniform buffers bound from the buffer cache
    FlushSubmit,  ///< Of FlushWork: the scheduler submits (Flush)
    CommandListFlush, ///< The front downloads GPU-written command list pages (FlushCommandPages)
    CommandListCopy,  ///< The front reads a GPU-written command list's copy (waits for the GPU)
    DrainAcquire,     ///< Of the split drains: semaphore acquires and their resumes (puller)
    DrainCommandList, ///< Of the split drains: GPU-written command lists without a copy
    DrainRun,         ///< Of the split drains: work that reads what the back wrote (CPU reads)
    DrainSync,        ///< Of the split drains: command lists that signal a sync fence
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "split writers", "split dma read",
    "update index", "update vertex", "update stages", "ubo reuse", "ubo copy", "ubo cached",
    "flush submit", "cmd list flush", "cmd list copy", "drain acquire", "drain cmd list",
    "drain run", "drain sync",
};

// Set at startup, or by System::Load when a gpu_profile file is in the user directory (Android
// has no environment to set). Enabled() runs several times per method write: a relaxed load.
inline std::atomic<bool> g_enabled{std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr};

inline bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 32> counters{}; ///< See Counter
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 sample_tick{}; ///< SampledScope: which constructions time themselves
    /// Calls of each engine method the back ran (CountMethod), by method number.
    std::unique_ptr<std::array<u32, 0x10000>> method_calls;
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

/// A zone entered millions of times a second: every call is counted, one in 2^Shift is timed
/// and its time scaled up, so the clock reads stop distorting what they measure. While not
/// timing, the scope is as if absent (nested zones count against the enclosing one).
template <u32 Shift>
class SampledScope {
public:
    explicit SampledScope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        ++t_data.count[static_cast<size_t>(zone)];
        timing = (t_data.sample_tick++ & ((u64{1} << Shift) - 1)) == 0;
        if (!timing) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Now();
    }

    ~SampledScope() {
        if (!enabled || !timing) {
            return;
        }
        const u64 elapsed = Now() - start;
        auto& data = t_data;
        const u64 own = elapsed > data.child_ns ? elapsed - data.child_ns : 0;
        data.exclusive_ns[static_cast<size_t>(zone)] += own << Shift;
        data.child_ns = parent_child_ns + elapsed;
    }

    SampledScope(const SampledScope&) = delete;
    SampledScope& operator=(const SampledScope&) = delete;

private:
    Zone zone;
    bool enabled;
    bool timing{};
    u64 start{};
    u64 parent_child_ns{};
};

/// An engine method the back ran (profiling only): the report lists the most called.
inline void CountMethod(u32 method, u32 amount = 1) {
    if (!Enabled()) {
        return;
    }
    auto& calls = t_data.method_calls;
    if (!calls) {
        calls = std::make_unique<std::array<u32, 0x10000>>();
        calls->fill(0);
    }
    (*calls)[method & 0xffff] += amount;
}

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
    MacroWrites,    ///< Of MethodWrites: made by macros running on the split's front
    MultiWords,     ///< Words the front sent to the back as multi-method entries
    GpfifoEntries,  ///< Command list (GPFIFO) entries started
    AcquireLedger,  ///< Of AcquireChecks: met without draining the back (semaphore ledger)
    JitSkippedWrites, ///< Of SameValueWrites: elided inside the ARM64 macro JIT
    BackSinkWrites,   ///< Back thread: ring method entries put in the engine's method sink
    BackMethodCalls,  ///< Back thread: ring method entries run through CallMethod (executable, direct)
    BackDeferredCalls, ///< Back thread: deferred calls run (Defer)
    UboReuseHits,     ///< Small uniform buffers bound to their last stream copy (bytes equal)
    UboCopyBytes,     ///< Bytes of small uniform buffers copied into the stream buffer
    Dispatches,       ///< Recorded chunks handed to the Vulkan worker (Scheduler::DispatchWork)
    WorkerWakes,      ///< Of Dispatches: the worker was asleep and was woken
    Submits,          ///< Scheduler::Flush from FlushWork
    CbBatchWords,     ///< Constant buffer words front macros sent batched (CB_DATA)
    CbBatchEntries,   ///< Ring entries those words went in
    CbRunWords,       ///< Pushbuffer constant buffer runs (offset + data words) sent as one entry
    CbRunEntries,     ///< Those entries
    CbBindsSkipped,   ///< Constant buffer binds not sent: same slot, buffer and size as last time
    CommandListFlushes,    ///< GPU-written command list ranges downloaded (one GPU wait each)
    CommandListPagesAlong, ///< Other command list pages downloaded in those waits
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
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value, {:.0f} from macros, {:.0f} elided in JIT)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window,
                        static_cast<double>(data.counters[8]) * 1e9 / window,
                        static_cast<double>(data.counters[12]) * 1e9 / window);
    line += fmt::format(" | multi words {:.0f} | GPFIFO entries {:.0f}",
                        static_cast<double>(data.counters[9]) * 1e9 / window,
                        static_cast<double>(data.counters[10]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited, {:.0f} met by ledger)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window,
                        static_cast<double>(data.counters[11]) * 1e9 / window);
    line += fmt::format(" | back entries: {:.0f} sunk, {:.0f} called, {:.0f} deferred calls",
                        static_cast<double>(data.counters[13]) * 1e9 / window,
                        static_cast<double>(data.counters[14]) * 1e9 / window,
                        static_cast<double>(data.counters[15]) * 1e9 / window);
    line += fmt::format(" | small UBOs: {:.0f} reused, {:.0f} KiB copied",
                        static_cast<double>(data.counters[16]) * 1e9 / window,
                        static_cast<double>(data.counters[17]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | dispatches {:.0f} ({:.0f} wakes), submits {:.0f} | CB words batched {:.0f} in {:.0f} entries",
                        static_cast<double>(data.counters[18]) * 1e9 / window,
                        static_cast<double>(data.counters[19]) * 1e9 / window,
                        static_cast<double>(data.counters[20]) * 1e9 / window,
                        static_cast<double>(data.counters[21]) * 1e9 / window,
                        static_cast<double>(data.counters[22]) * 1e9 / window);
    line += fmt::format(" | CB runs {:.0f} words in {:.0f} entries, binds skipped {:.0f}",
                        static_cast<double>(data.counters[23]) * 1e9 / window,
                        static_cast<double>(data.counters[24]) * 1e9 / window,
                        static_cast<double>(data.counters[25]) * 1e9 / window);
    line += fmt::format(" | cmd list flushes {:.0f} ({:.0f} pages along)",
                        static_cast<double>(data.counters[26]) * 1e9 / window,
                        static_cast<double>(data.counters[27]) * 1e9 / window);
    if (data.method_calls) {
        // The ten most called engine methods (per second).
        std::array<std::pair<u32, u32>, 10> top{};
        for (u32 method = 0; method < 0x10000; ++method) {
            const u32 calls = (*data.method_calls)[method];
            if (calls <= top.back().second) {
                continue;
            }
            size_t at = top.size() - 1;
            while (at > 0 && top[at - 1].second < calls) {
                top[at] = top[at - 1];
                --at;
            }
            top[at] = {method, calls};
        }
        line += " | methods called:";
        for (const auto& [method, calls] : top) {
            if (calls == 0) {
                break;
            }
            line += fmt::format(" {:#x} {:.0f}", method, static_cast<double>(calls) * 1e9 / window);
        }
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    auto method_calls = std::move(data.method_calls);
    data = {};
    if (method_calls) {
        method_calls->fill(0);
        data.method_calls = std::move(method_calls);
    }
    data.window_start = now;
}

/// An MME DMA read that flushed GPU-written memory (each one waits for the host GPU). Logged at
/// most every 10 s, profile or not: how many, the bytes they read and the time they waited.
inline void NoteDmaReadFlush(u64 elapsed_ns, u64 bytes) {
    struct Window {
        u64 start{};
        u64 flushes{};
        u64 ns{};
        u64 bytes{};
    };
    static Window window; // the thread running the macros
    const u64 now = Now();
    if (window.start == 0) {
        window.start = now;
    }
    ++window.flushes;
    window.ns += elapsed_ns;
    window.bytes += bytes;
    if (now - window.start >= 10'000'000'000ULL) {
        LOG_INFO(HW_GPU, "MME DMA reads that flushed GPU-written memory in {:.1f} s: {} ({} bytes), "
                         "{:.1f} ms waiting",
                 static_cast<double>(now - window.start) / 1e9, window.flushes, window.bytes,
                 static_cast<double>(window.ns) / 1e6);
        window = {};
    }
}

} // namespace VideoCommon::Nx2Profile
