// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

namespace Service {

class MkwMovieActivity {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr bool IsLargeMovie(u64 program_id, u32 width) {
        return program_id == 0x0400C3F00006E000ULL && width >= 1920;
    }

    void RecordFrame(Clock::time_point now) {
        last_frame.store(Ticks(now), std::memory_order_relaxed);
    }

    void Reset() {
        last_frame.store(0, std::memory_order_relaxed);
    }

    bool Active(Clock::time_point now) const {
        const s64 last = last_frame.load(std::memory_order_relaxed);
        const s64 elapsed = Ticks(now) - last;
        return last != 0 && elapsed >= 0 && elapsed < 250'000'000;
    }

private:
    static s64 Ticks(Clock::time_point now) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    std::atomic<s64> last_frame{};
};

} // namespace Service
