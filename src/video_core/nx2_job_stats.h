// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2JobStats {

// EDEN_NX2_GPU_JOB_STATS=1: what the nx2:gpu memory jobs cost, logged every 5 seconds.
enum class Kind : size_t {
    Register,   ///< handler of 101 (CreateGpuAccessibleMemory)
    Map,        ///< handler of 211 (NormalMapGpuMemory4K)
    Unmap,      ///< handler of 213 and 102 (unmap, destroy)
    SignalWait, ///< from queueing an unmap/destroy's event to the back running it: what the guest waits
    FrontDrain, ///< the split front draining the back for a GPU map/unmap command
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> names{
    "register 101", "map 211", "unmap 213/destroy 102 handler", "unmap/destroy event wait",
    "front drain on map/unmap"};

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_GPU_JOB_STATS") != nullptr;
    return enabled;
}

struct Slot {
    std::atomic<u64> count{};
    std::atomic<u64> total_ns{};
    std::atomic<u64> max_ns{};
};
inline std::array<Slot, static_cast<size_t>(Kind::Count)> slots;
inline std::atomic<s64> next_report_ns{};

inline void Note(Kind kind, std::chrono::nanoseconds elapsed) {
    auto& slot = slots[static_cast<size_t>(kind)];
    const u64 ns = static_cast<u64>(elapsed.count());
    slot.count.fetch_add(1, std::memory_order_relaxed);
    slot.total_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 seen = slot.max_ns.load(std::memory_order_relaxed);
    while (ns > seen && !slot.max_ns.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
    constexpr s64 interval = 5'000'000'000;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 due = next_report_ns.load(std::memory_order_relaxed);
    if (now < due || !next_report_ns.compare_exchange_strong(due, now + interval)) {
        return;
    }
    std::string line;
    for (size_t i = 0; i < slots.size(); ++i) {
        const u64 n = slots[i].count.exchange(0, std::memory_order_relaxed);
        const u64 total = slots[i].total_ns.exchange(0, std::memory_order_relaxed);
        const u64 max = slots[i].max_ns.exchange(0, std::memory_order_relaxed);
        line += fmt::format("{}{}: {} calls, avg {:.2f} ms, max {:.2f} ms", i ? "; " : "", names[i],
                            n, n ? 1e-6 * static_cast<double>(total) / static_cast<double>(n) : 0.0,
                            1e-6 * static_cast<double>(max));
    }
    if (due != 0) { // the first call only starts the window
        LOG_INFO(HW_GPU, "nx2:gpu jobs, last 5 s: {}", line);
    }
}

/// Times its scope when the statistics are on.
class Timer {
public:
    explicit Timer(Kind kind_) : kind{kind_}, enabled{Enabled()} {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~Timer() {
        if (enabled) {
            Note(kind, std::chrono::steady_clock::now() - start);
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Kind kind;
    bool enabled;
    std::chrono::steady_clock::time_point start{};
};


// EDEN_NX2_COND_STATS=1: why the host conditional-rendering path gave up (see ProcessQueryCondition).
enum class CondReason : size_t {
    Override, ///< render_enable_override is not UseRenderEnable
    TrueFalse, ///< mode True/False: nothing to compare
    NoExtension, ///< no VK_EXT_conditional_rendering
    FoundQuery, ///< a compared address is a tracked query (counted per address, 0..2 per condition)
    FoundBuffer, ///< a compared address is GPU-modified in the buffer cache
    NoneTracked, ///< neither address is a query or GPU-written buffer
    NotDirty, ///< tracked, but nothing newer than the guest's copy
    Accepted, ///< the host path took the condition
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(CondReason::Count)> cond_names{
    "override", "true/false", "no extension", "query found", "buffer found", "none tracked",
    "not dirty", "accepted"};
inline std::array<std::atomic<u64>, static_cast<size_t>(CondReason::Count)> cond_reasons;
[[nodiscard]] inline bool CondStatsEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_COND_STATS") != nullptr;
    return enabled;
}
inline void CondNote(CondReason reason) {
    if (CondStatsEnabled()) {
        cond_reasons[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace VideoCommon::Nx2JobStats
