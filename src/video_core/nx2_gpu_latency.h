// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"

// A Switch 2 GPU starts on a command list some time after the game submits it, and games rely on
// that: Hyrule Warriors: Age of Imprisonment writes the vertices and indices of new UI labels about
// 0.5-2 ms after submitting the draws that use them (one frame of black tiles and missing text on
// the map otherwise). The GPU thread holds each list until it is this old, unless a game thread
// blocks on the GPU first (then it isn't writing). It only waits when it has caught up with the
// game; a list that has queued that long already runs at once.
// The hold is only on while the game runs steadily at 30 or 20 fps (it waits for vblank, so the
// hold takes time it would otherwise sleep). In a frame-rate-bound scene the game, GPU thread and
// back run one after another and the hold would lengthen every frame, so it stays off there.
namespace VideoCommon::Nx2GpuLatency {

/// Bumped whenever a game thread starts waiting for GPU progress.
inline std::atomic<u64> g_guest_waits{0};

inline void NoteGuestWait() {
    g_guest_waits.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] inline u64 GuestWaits() {
    return g_guest_waits.load(std::memory_order_acquire);
}

/// EDEN_NX2_GPU_LATENCY_US=<n> sets the hold (0 turns it off).
[[nodiscard]] inline std::chrono::microseconds Latency() {
    static const std::chrono::microseconds latency{[] {
        const char* const value = std::getenv("EDEN_NX2_GPU_LATENCY_US");
        return value != nullptr ? std::strtol(value, nullptr, 10) : 3000L;
    }()};
    return latency;
}

/// Whether the game's frame pacing is steady enough to hold lists (see above).
inline std::atomic<bool> g_steady{false};

[[nodiscard]] inline bool Steady() {
    return g_steady.load(std::memory_order_relaxed);
}

/// Called on each guest present. Over the last 60 frame intervals, the hold turns on when their
/// median is 2 or 3 vblanks and 54 are within 4 ms of it, and off below 45 (a lone hitch doesn't
/// switch it). Turning off (other than for a loading stall) doubles how many frames it waits
/// before turning on again, up to about 30 s of frames, in case the hold itself made the frames
/// slower; 30 s on halves it.
namespace Detail {
constexpr size_t Window = 60;
struct Pacing {
    std::chrono::steady_clock::time_point last, on_since;
    std::array<double, Window> intervals{};
    size_t count = 0;
    u32 since_off = 0, need = 60, drops = 0;
};
inline std::mutex g_pacing_mutex;
inline Pacing g_pacing;
} // namespace Detail

/// Forgets the previous emulation session's pacing (called with the other Switch 2 resets).
inline void Reset() {
    std::scoped_lock lock{Detail::g_pacing_mutex};
    Detail::g_pacing = {};
    g_steady.store(false, std::memory_order_relaxed);
}

inline void NotePresent() {
    using Clock = std::chrono::steady_clock;
    using Detail::Window;
    std::scoped_lock lock{Detail::g_pacing_mutex};
    auto& [last, on_since, intervals, count, since_off, need, drops] = Detail::g_pacing;
    const auto now = Clock::now();
    intervals[count++ % Window] = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++since_off;
    if (count < Window) {
        return;
    }
    std::array<double, Window> sorted = intervals;
    std::nth_element(sorted.begin(), sorted.begin() + Window / 2, sorted.end());
    const double median = sorted[Window / 2];
    const auto in_band = std::count_if(intervals.begin(), intervals.end(),
                                       [median](double v) { return std::abs(v - median) <= 4.0; });
    // Paced to every 2nd or 3rd vblank (30 or 20 fps), there is room for the hold; at 60 frame
    // times can't tell how much, and a scene running slow at 25-30 ms has none.
    constexpr double Vblank = 16.683;
    const bool slow_cap =
        std::abs(median - 2 * Vblank) <= 1.5 || std::abs(median - 3 * Vblank) <= 1.5;
    const bool on = g_steady.load(std::memory_order_relaxed);
    if (!on && slow_cap && in_band >= 54 && since_off >= need) {
        g_steady.store(true, std::memory_order_relaxed);
        on_since = now;
        if (drops < 32) {
            LOG_INFO(HW_GPU, "nx2 gpu latency: on ({} of {} frames near {:.1f} ms)", in_band,
                     Window, median);
        }
    } else if (on && (in_band < 45 || !slow_cap)) {
        g_steady.store(false, std::memory_order_relaxed);
        since_off = 0;
        const bool stall = std::any_of(intervals.begin(), intervals.end(),
                                       [](double v) { return v > 100.0; });
        if (!stall) {
            need = (std::min)(need * 2, 1920u);
        }
        if (++drops <= 32) {
            LOG_INFO(HW_GPU, "nx2 gpu latency: off ({} of {} frames near {:.1f} ms{}), next try "
                             "after {} frames", in_band, Window, median,
                     stall ? ", loading" : "", need);
        }
    } else if (on && need > 60 && now - on_since > std::chrono::seconds{30}) {
        need /= 2;
        on_since = now;
    }
}

} // namespace VideoCommon::Nx2GpuLatency
