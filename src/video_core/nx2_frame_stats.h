// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <span>

#include "common/common_types.h"

// Nx2Frame: per-second percentiles of the guest present-to-present interval and of the time vi2
// is blocked on the GPU fence, logged from the vi2 present path. EDEN_NX2_FRAME_STATS=0 turns
// the line off. Fixed-size storage only: nothing is allocated per present.
// Nearest-rank p99 equals max for windows under 100 samples (every window at 60 FPS or less), so
// tooling that pools runs should use iv_n_over_20ms, a count of intervals over 20 ms, not p99.
namespace VideoCommon::Nx2FrameStats {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FRAME_STATS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

struct Percentiles {
    u32 p50{};
    u32 p90{};
    u32 p99{};
    u32 max{};
};

/// Nearest-rank percentiles of values, which it reorders. All zero when values is empty.
inline Percentiles Compute(std::span<u32> values) {
    if (values.empty()) {
        return {};
    }
    const auto at = [values](size_t percent) {
        const size_t index = (percent * values.size() + 99) / 100 - 1;
        std::nth_element(values.begin(), values.begin() + index, values.end());
        return values[index];
    };
    Percentiles result;
    result.p50 = at(50);
    result.p90 = at(90);
    result.p99 = at(99);
    result.max = *std::max_element(values.begin(), values.end());
    return result;
}

/// Microseconds as milliseconds, for the log line.
inline double Ms(u32 us) {
    return static_cast<double>(us) / 1000.0;
}

/// Samples of one window. Past N samples the oldest are overwritten; max stays exact.
template <size_t N>
class Ring {
public:
    void Push(u32 value) {
        values[count % N] = value;
        ++count;
        max = (std::max)(max, value);
    }

    u32 Count() const {
        return count;
    }

    /// Percentiles of this window, computed in scratch; then starts a new window.
    Percentiles Take(std::array<u32, N>& scratch) {
        const size_t size = (std::min<size_t>)(count, N);
        std::copy_n(values.begin(), size, scratch.begin());
        Percentiles result = Compute(std::span<u32>{scratch.data(), size});
        result.max = max;
        count = 0;
        max = 0;
        return result;
    }

private:
    std::array<u32, N> values{};
    u32 count{};
    u32 max{};
};

template <size_t N = 512>
class Window {
public:
    using Clock = std::chrono::steady_clock;

    struct Summary {
        u32 presents{};
        Percentiles interval_us;
        Percentiles fence_us;
        u32 intervals_over_20ms{};
    };

    /// Records a present that entered vi2 at `entered` and was blocked `fence_wait` on the GPU
    /// fence. True when this present closes a window of at least `period`; Last() then holds it.
    bool Add(Clock::time_point entered, Clock::duration fence_wait,
             Clock::duration period = std::chrono::seconds{1}) {
        if (last_present == Clock::time_point{}) {
            window_start = entered;
        } else {
            const u32 interval = ToUs(entered - last_present);
            intervals.Push(interval);
            intervals_over_20ms += interval > 20'000 ? 1 : 0;
        }
        last_present = entered;
        fences.Push(ToUs(fence_wait));
        if (entered - window_start < period) {
            return false;
        }
        summary.presents = fences.Count();
        summary.interval_us = intervals.Take(scratch);
        summary.fence_us = fences.Take(scratch);
        summary.intervals_over_20ms = intervals_over_20ms;
        intervals_over_20ms = 0;
        window_start = entered;
        return true;
    }

    const Summary& Last() const {
        return summary;
    }

private:
    static u32 ToUs(Clock::duration duration) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        return static_cast<u32>(std::clamp<long long>(
            us, 0, static_cast<long long>((std::numeric_limits<u32>::max)())));
    }

    Ring<N> intervals;
    Ring<N> fences;
    std::array<u32, N> scratch{};
    Summary summary;
    u32 intervals_over_20ms{};
    Clock::time_point window_start{};
    Clock::time_point last_present{};
};

} // namespace VideoCommon::Nx2FrameStats
