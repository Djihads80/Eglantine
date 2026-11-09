// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include "common/common_types.h"

namespace VideoCommon::Nx2Timeline {

// EDEN_NX2_TIMELINE=1 (diagnostic): follows every command list the guest submits through the GPU
// thread: when it was submitted, when the front finished parsing it and when the back finished
// running it. The present statistics read them to show where the guest-to-fence time goes.

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_TIMELINE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

constexpr size_t Entries = size_t{1} << 15;

struct Stamps {
    std::array<std::atomic<u64>, Entries> submit{};
    std::array<std::atomic<u64>, Entries> front{};
    std::array<std::atomic<u64>, Entries> back{};
    std::array<std::atomic<u32>, Entries> words{};
    std::array<std::atomic<u64>, Entries> front_cpu{}; ///< Thread CPU time at front done
    std::array<std::atomic<u64>, Entries> back_cpu{};  ///< ... and at back done
    std::atomic<u64> submitted{0};
};
inline Stamps g_stamps;

[[nodiscard]] inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Guest thread: a command list was submitted. Returns its number (0 when off).
[[nodiscard]] inline u64 OnSubmit(u32 words) {
    if (!Enabled()) {
        return 0;
    }
    const u64 seq = g_stamps.submitted.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t index = seq % Entries;
    g_stamps.front[index].store(0, std::memory_order_relaxed);
    g_stamps.back[index].store(0, std::memory_order_relaxed);
    g_stamps.words[index].store(words, std::memory_order_relaxed);
    g_stamps.submit[index].store(Now(), std::memory_order_release);
    return seq;
}

[[nodiscard]] inline u64 ThreadCpuNow() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
}

inline void OnFront(u64 seq) {
    if (seq != 0) {
        g_stamps.front_cpu[seq % Entries].store(ThreadCpuNow(), std::memory_order_relaxed);
        g_stamps.front[seq % Entries].store(Now(), std::memory_order_release);
    }
}

inline void OnBack(u64 seq) {
    if (seq != 0) {
        g_stamps.back_cpu[seq % Entries].store(ThreadCpuNow(), std::memory_order_relaxed);
        g_stamps.back[seq % Entries].store(Now(), std::memory_order_release);
    }
}

/// Averages over the presents since the last report, all in milliseconds.
class FrameStats {
public:
    /// `enter`/`waited`: when the present was entered and when its fence wait returned.
    template <typename Log>
    void OnPresent(u64 enter, u64 waited, Log&& log) {
        if (!Enabled()) {
            return;
        }
        const u64 last = g_stamps.submitted.load(std::memory_order_acquire);
        if (previous_return != 0 && last > previous_seq && last - previous_seq < Entries / 2) {
            const auto at = [](const auto& array, u64 seq) {
                return array[seq % Entries].load(std::memory_order_acquire);
            };
            const u64 first_submit = at(g_stamps.submit, previous_seq + 1);
            const u64 last_submit = at(g_stamps.submit, last);
            const u64 last_front = at(g_stamps.front, last);
            const u64 last_back = at(g_stamps.back, last);
            u64 front_busy = 0;
            u64 back_busy = 0;
            u64 words = 0;
            u64 previous_front = 0;
            u64 previous_back = 0;
            for (u64 seq = previous_seq + 1; seq <= last; ++seq) {
                const u64 submitted = at(g_stamps.submit, seq);
                const u64 fronted = at(g_stamps.front, seq);
                const u64 backed = at(g_stamps.back, seq);
                words += g_stamps.words[seq % Entries].load(std::memory_order_relaxed);
                if (fronted != 0) {
                    front_busy += fronted - (std::max)(submitted, previous_front);
                    previous_front = fronted;
                }
                if (backed != 0 && fronted != 0) {
                    back_busy += backed - (std::max)(fronted, previous_back);
                    previous_back = backed;
                }
            }
            if (last_front != 0 && last_back != 0) {
                const u64 front_cpu_now = at(g_stamps.front_cpu, last);
                const u64 back_cpu_now = at(g_stamps.back_cpu, last);
                if (previous_front_cpu != 0) {
                    front_cpu_total += front_cpu_now - previous_front_cpu;
                    back_cpu_total += back_cpu_now - previous_back_cpu;
                }
                previous_front_cpu = front_cpu_now;
                previous_back_cpu = back_cpu_now;
                ++frames;
                lists += last - previous_seq;
                Add(guest_before_first, first_submit, previous_return);
                Add(last_submit_before_present, enter, last_submit);
                lag_front += Signed(last_front, enter);
                lag_back += Signed(last_back, enter);
                Add(wake_after_back, waited, last_back);
                total_wait += waited - enter;
                front_ms += front_busy;
                back_ms += back_busy;
                total_words += words;
                frame_time += enter - previous_enter;
            }
        }
        previous_seq = last;
        previous_enter = enter;
        previous_return = Now();
        if (frames >= 300) {
            const double n = static_cast<double>(frames);
            const auto ms = [n](u64 sum) { return static_cast<double>(sum) / 1e6 / n; };
            log(frames, static_cast<double>(lists) / n, static_cast<double>(total_words) / n,
                ms(frame_time), ms(guest_before_first), ms(last_submit_before_present),
                static_cast<double>(lag_front) / 1e6 / n, static_cast<double>(lag_back) / 1e6 / n,
                ms(wake_after_back), ms(total_wait), ms(front_cpu_total), ms(back_cpu_total));
            const u64 keep_front = previous_front_cpu;
            const u64 keep_back = previous_back_cpu;
            *this = FrameStats{};
            previous_front_cpu = keep_front;
            previous_back_cpu = keep_back;
            previous_seq = last;
            previous_enter = enter;
            previous_return = previous_return;
        }
    }

private:
    static void Add(u64& sum, u64 later, u64 earlier) {
        sum += later > earlier ? later - earlier : 0;
    }
    static s64 Signed(u64 later, u64 earlier) {
        return static_cast<s64>(later) - static_cast<s64>(earlier);
    }

    u64 previous_seq{};
    u64 previous_enter{};
    u64 previous_return{};
    u64 frames{};
    u64 lists{};
    u64 total_words{};
    u64 frame_time{};
    u64 guest_before_first{};
    u64 last_submit_before_present{};
    s64 lag_front{};
    s64 lag_back{};
    u64 wake_after_back{};
    u64 total_wait{};
    u64 front_ms{};
    u64 back_ms{};
    u64 front_cpu_total{};
    u64 back_cpu_total{};
    u64 previous_front_cpu{};
    u64 previous_back_cpu{};
};

} // namespace VideoCommon::Nx2Timeline
