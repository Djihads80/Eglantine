// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "common/common_types.h"
#include "core/hardware_properties.h"

// EDEN_NCE_STATS=1: what the guest CPU cores spend their host time on, logged every 10 s: time in
// guest code, SVCs, other host work and idle per core; SVCs by number; guest memory faults by
// kind and their handling time; interrupts; page protection churn; and every host thread's CPU
// time and run-queue wait. Off, each hook costs one relaxed load.
namespace Core::NceStats {

inline std::atomic<bool> g_enabled{false};

inline bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

/// Reads the switch (after env.txt has been applied) and starts the reporter, once.
void Init();

/// A cheap timestamp: the virtual counter on ARM64, nanoseconds elsewhere.
inline u64 Ticks() {
#if defined(__aarch64__)
    u64 ticks;
    asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
#endif
}

/// What a core's host thread is doing.
enum class State : u32 { Host, Guest, Idle, Svc, Count };

/// Guest faults that the NCE signal handlers take.
enum class Fault : u32 {
    Rasterizer,     ///< A write to a page the GPU caches: the GPU caches are invalidated
    RasterizerWait, ///< ... which waited for the GPU thread to do it (GPU-written page)
    Emulated,       ///< ... or the store was emulated, as Dynarmic does (EDEN_NCE_EMULATE_WRITES)
    SeparateHeap,   ///< A separate-heap page the heap tracker had unmapped is mapped back in
    Mapped,       ///< A mapped page with nothing to fix (protection already lifted)
    Unhandled,    ///< An unmapped address: the access is skipped or the thread stops
    Alignment,    ///< SIGBUS: the instruction is interpreted
    ReadTrap,     ///< A read of a GPU-written page: its GPU writes are downloaded (read trap)
    Count,
};

constexpr size_t MaxCores = Core::Hardware::NUM_CPU_CORES;
constexpr size_t SvcSlots = 0x100;

struct alignas(64) CoreCounters {
    // Written only by the host thread of this core (Add), read by the reporter.
    std::array<std::atomic<u64>, static_cast<size_t>(State::Count)> state_ticks{};
    std::array<std::atomic<u64>, static_cast<size_t>(Fault::Count)> fault_count{};
    std::array<std::atomic<u64>, static_cast<size_t>(Fault::Count)> fault_ticks{};
    std::atomic<u64> fault_max_ticks{};
    std::atomic<u64> runs{};           ///< Entries into guest code
    std::atomic<u64> signal_entries{}; ///< ... of which by a signal (no post-SVC trampoline)
    std::atomic<u64> svc_exits{};
    std::atomic<u64> interrupt_exits{};
    std::atomic<u64> idle_calls{};
    std::atomic<u64> idle_wakes{};     ///< Idle calls that slept until an interrupt
    std::atomic<u64> wake_ticks{};     ///< Interrupt to the woken core running again
    std::atomic<u64> wake_max_ticks{};
    std::array<std::atomic<u64>, SvcSlots> svc_count{};
    std::array<std::atomic<u64>, SvcSlots> svc_ticks{};
    // GPU-cached page write faults: the shape of the writes.
    std::atomic<u64> sequential_faults{}; ///< On the page after this core's previous one
    std::atomic<u64> same_page_faults{};  ///< On the same page again
    std::atomic<u64> scalar_faults{};     ///< By a single-register store (else pair/vector/atomic)
    std::atomic<u64> ahead_pages{};       ///< Pages invalidated ahead (EDEN_NCE_FAULT_AHEAD)
    std::atomic<u64> emulate_fallbacks{}; ///< Stores not emulated: budget used up or unsupported

    // Written by any thread.
    std::atomic<u64> interrupt_signals{}; ///< Interrupts that stopped running guest code
    std::atomic<u64> interrupt_idle{};    ///< Interrupts of a core that ran no guest code

    // The host thread's own bookkeeping.
    u64 since{};
    State state{State::Host};
    u32 svc{};
};

CoreCounters& Counters(size_t core);

/// Set when GPU::InvalidateRegion waited for the GPU thread on this thread: the fault handler
/// reads and clears it.
inline thread_local bool t_invalidate_waited = false;

/// Memory-tracking counters, written by any thread.
struct SharedCounters {
    std::atomic<u64> cache_protects{};      ///< RasterizerMarkRegionCached protection changes
    std::atomic<u64> cache_protect_bytes{};
    std::atomic<u64> cache_protect_ticks{};
    std::atomic<u64> cache_protects_cores{}; ///< ... by the CPU core threads (the rest: GPU threads)
    std::atomic<u64> cache_protect_ticks_cores{};
    std::atomic<u64> gpu_invalidate_fast{}; ///< CPU write invalidations done in place
    std::atomic<u64> gpu_invalidate_sync{}; ///< ... that waited for the GPU thread instead
    std::atomic<u64> gpu_invalidate_sync_ticks{};
};

SharedCounters& Shared();

/// For counters with a single writer: no read-modify-write instruction needed.
inline void Add(std::atomic<u64>& counter, u64 value) {
    counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

inline void Max(std::atomic<u64>& counter, u64 value) {
    if (value > counter.load(std::memory_order_relaxed)) {
        counter.store(value, std::memory_order_relaxed);
    }
}

/// Moves the time account of `core` (the calling host thread's core) to `next`.
inline void Enter(size_t core, State next, u32 svc = 0) {
    if (core >= MaxCores) {
        return;
    }
    CoreCounters& c = Counters(core);
    const u64 now = Ticks();
    if (c.since != 0) {
        const u64 elapsed = now - c.since;
        Add(c.state_ticks[static_cast<size_t>(c.state)], elapsed);
        if (c.state == State::Svc) {
            Add(c.svc_ticks[c.svc], elapsed);
        }
    }
    c.since = now;
    c.state = next;
    if (next == State::Svc) {
        c.svc = svc % SvcSlots;
        Add(c.svc_count[c.svc], 1);
    }
}

inline void RecordFault(size_t core, Fault kind, u64 ticks) {
    if (core >= MaxCores) {
        return;
    }
    CoreCounters& c = Counters(core);
    Add(c.fault_count[static_cast<size_t>(kind)], 1);
    Add(c.fault_ticks[static_cast<size_t>(kind)], ticks);
    Max(c.fault_max_ticks, ticks);
}

} // namespace Core::NceStats
