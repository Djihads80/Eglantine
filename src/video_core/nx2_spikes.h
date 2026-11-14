// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

// Frame spike recorder (EDEN_NX2_SPIKES=1, or =<ms> for the threshold; the MCP frame_spikes tool
// turns it on and off live). A frame is the time between two guest presents (nx2:vi2 229). A
// frame over the threshold and over `ratio` times the recent median is kept with what happened
// during it: the present's own split (the game's time, the GPU fence wait, composite, pacing),
// each GPU thread's profiler zones, the CPU time and run-queue wait of every host thread (Linux),
// the process's page faults and file reads, pipeline builds, skipped draws and the log lines.
// Each spike is also logged as one line.
namespace VideoCommon::Nx2Spikes {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool Enabled();

/// Turns recording on or off. On, it also turns on the GPU profiler (and turns it off again with
/// the recorder if it was off) and the in-memory log ring.
void SetEnabled(bool enabled);

/// A spike is a frame over `min_ms` and over `ratio` times the median of the last 120 frames.
void Configure(double min_ms, double ratio);

/// Called at every guest present: when the present began, when the GPU fence wait, composite and
/// vblank pacing ended.
void OnPresent(Clock::time_point entered, Clock::time_point waited, Clock::time_point composited,
               Clock::time_point paced);

enum class Event : u32 {
    DriverBuild,     ///< A vkCreate*Pipelines call (any thread)
    DriverCompile,   ///< Of those, over 10 ms: compiled, not from the driver's cache
    RuntimePipeline, ///< A pipeline translated and built on the GPU thread
    DrawSkipped,     ///< A draw skipped while its pipeline translates
    CpuWriteSlowPath,   ///< Guest CPU writes to GPU-cached pages (amount: bytes)
    CpuWriteInvalidate, ///< Of those, ones that reached the GPU caches (OnCPUWrite)
    CpuWriteLockWait,   ///< OnCPUWrite waiting for the buffer or texture cache lock
    ProtectChange,      ///< Host page protection changes for the GPU caches (amount: pages)
    ReadTrapHit,        ///< Guest CPU reads that hit a read trap
    JitCompile,         ///< Guest code blocks compiled (polled from the JIT)
    JitFastmemRecompile, ///< Blocks recompiled after a fastmem fault (polled from the JIT)
    JitInvalidate,      ///< Guest code cache range invalidations (amount: bytes)
    JitClear,           ///< Whole guest code cache clears
    Svc,                ///< Every SVC (time: inside the call, blocked time included)
    SvcSleep,           ///< SleepThread, yields included
    SvcWaitSync,        ///< WaitSynchronization
    SvcMutexCondvar,    ///< ArbitrateLock/Unlock, WaitProcessWideKeyAtomic, SignalProcessWideKey
    SvcAddressArbiter,  ///< WaitForAddress, SignalToAddress
    SvcIpc,             ///< SendSyncRequest(WithUserBuffer), ReplyAndReceive(WithUserBuffer)
    Count,
};

/// True while the recorder or a Window counts events: callers can skip timing work otherwise.
[[nodiscard]] bool Counting();

/// Counts an event (any thread), with the time it took and an amount when there is one.
void Note(Event event, Clock::duration time = {}, u64 amount = 0);

/// An event counted elsewhere (the JIT's own totals): read at each present and window instead of
/// counted with Note. The functions return running totals.
void SetPolled(Event event, u64 (*count)(), u64 (*ns)());

/// A guest thread's CPU ticks so far.
struct GuestThreadTicks {
    u64 id{};
    u64 ticks{};
    s32 core{};
};

/// Where guest thread CPU times come from (set by the core while a program runs).
struct GuestThreadSource {
    /// Every guest thread's CPU ticks and the guest clock's ticks now; false when there is no
    /// program.
    bool (*read)(void* user, std::vector<GuestThreadTicks>& threads, u64& clock){};
    /// A thread's name ("" when gone).
    std::string (*name)(void* user, u64 id){};
    void* user{};
};

/// Sets or (with an empty source) clears the guest thread source; clearing waits for readers.
void SetGuestThreadSource(GuestThreadSource source);

struct ZoneTime {
    std::string zone;
    double ms{};
    u64 calls{};
};

struct GpuThreadTimes {
    std::string thread;
    /// Costliest first. "other (no zone)" is time in no zone, or in one still running at the
    /// present (it counts when it ends, in the next frame).
    std::vector<ZoneTime> zones;
    std::vector<std::pair<std::string, u64>> counters;
};

struct HostThreadTimes {
    std::string name;
    int tid{};
    double cpu_ms{};       ///< Time on a CPU
    double run_queue_ms{}; ///< Time runnable but waiting for a CPU
};

struct EventTimes {
    std::string event;
    u64 count{};
    double ms{};
    u64 amount{};
    const char* unit{}; ///< What amount counts, or null when the event has none
};

struct GuestThreadTimes {
    u64 id{};
    std::string name;
    s32 core{};
    double cpu_ms{}; ///< Guest CPU time (the kernel's accounting, scaled to host ms)
};

struct Spike {
    u64 number{};
    u64 present{};
    double at_s{}; ///< Seconds since the recorder started, at the end of the frame
    double frame_ms{};
    double median_ms{};
    // The frame runs from the previous present's start to this one's: the previous present's
    // GPU fence wait, composite and pacing, then the game until it presents again. They add up
    // to frame_ms.
    double game_ms{};      ///< From the previous present's return to this present
    double fence_ms{};     ///< The previous present waiting for the GPU thread
    double composite_ms{};
    double pace_ms{};
    std::vector<GpuThreadTimes> gpu;
    bool host_sampled{};       ///< Host thread times available (Linux)
    double host_span_ms{};     ///< What the host samples covered (they bracket the frame)
    std::vector<HostThreadTimes> host; ///< Busiest first, and any that waited for a CPU
    u64 minor_faults{};
    u64 major_faults{};
    u64 read_kib{};
    std::vector<EventTimes> events;
    std::vector<GuestThreadTimes> guest; ///< Busiest guest threads first
    std::vector<std::string> log; ///< Log lines written during the frame (the last ones)
};

struct Report {
    bool enabled{};
    double min_ms{};
    double ratio{};
    u64 presents{};
    double median_ms{};
    u64 spikes{};
    std::vector<Spike> recent; ///< Newest first
    std::vector<Spike> worst;  ///< Longest first
};

[[nodiscard]] Report Read(size_t limit);

/// What happened over a measured window (perf_sample).
struct WindowReport {
    double seconds{};
    std::vector<EventTimes> events;
    std::vector<GuestThreadTimes> guest;
    bool host_sampled{};
    std::vector<HostThreadTimes> host;
    u64 minor_faults{};
    u64 major_faults{};
    u64 read_kib{};
};

/// Counts events from construction to Finish, independently of the recorder.
class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// The events, busiest guest and host threads and page faults since construction.
    [[nodiscard]] WindowReport Finish(size_t max_threads = 12);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void Clear();

/// The spike as the one line the recorder logs.
[[nodiscard]] std::string Summary(const Spike& spike);

} // namespace VideoCommon::Nx2Spikes
