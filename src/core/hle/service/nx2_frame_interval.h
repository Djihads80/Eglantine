// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>

#include "common/common_types.h"
#include "common/game_presets.h"

namespace Service::Nx2FrameInterval {

// Hyrule Warriors: Age of Imprisonment advances its simulation by a whole number of 60 Hz frames
// per frame, read from a global, and waits that many vblanks itself. The global is 1 for one
// player and 2 in split screen, so below 60 fps one player runs in slow motion. The loader finds
// the global (and the value the game copies into it) next to its single copy; with
// hyrule_dynamic_fps the present path switches it to 2 while the game runs below about 50 fps.
inline constexpr u64 HyruleWarriorsProgramId = Settings::HyruleWarriorsProgramId;

/// Picks 1 (60 fps) or 2 (30 fps) from the time between presents.
class Controller {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double, std::milli>;

    /// nominal: one 60 Hz frame at the current speed limit.
    [[nodiscard]] u32 Update(Clock::time_point now, Duration nominal) {
        if (last == Clock::time_point{}) {
            last = now;
            return interval;
        }
        const Duration frame = now - last;
        last = now;
        if (frame > MaxFrame) {
            // Loading or a pause: not a frame rate to react to.
            ResetWindow();
            probe_frames = 0;
            return interval;
        }
        if (probing) {
            Probe(now, frame, nominal);
        } else if (interval == 1) {
            window_sum += frame - window[window_next];
            window[window_next] = frame;
            window_next = (window_next + 1) % window.size();
            window_count = (std::min)(window_count + 1, window.size());
            if (window_count == window.size() && window_sum / window.size() > nominal * DropAbove) {
                interval = 2;
                probe_delay = FirstProbeDelay;
                next_probe = now + probe_delay;
            }
        } else if (now >= next_probe) {
            // At 2 the game waits for the second vblank, so only trying 1 shows whether it fits.
            interval = 1;
            probing = true;
            probe_frames = 0;
            probe_sum = {};
        }
        return interval;
    }

    [[nodiscard]] u32 Interval() const {
        return interval;
    }

private:
    // A few percent of slow motion beats halving the frame rate: only below about 50 fps.
    static constexpr Duration MaxFrame{100.0};
    static constexpr double DropAbove = 1.2;      // mean of the window (50 fps)
    static constexpr double KeepAtOrBelow = 1.12; // mean of a probe (about 53.5 fps)
    static constexpr double GiveUpAbove = 1.5;    // mean of a probe so far (40 fps)
    static constexpr int ProbeSkip = 2;           // frames the switch itself takes
    static constexpr int ProbeFrames = 30;
    static constexpr int ProbeEarliestGiveUp = 8; // single frames spike: judge means only
    static constexpr std::chrono::seconds FirstProbeDelay{2};
    static constexpr std::chrono::seconds MaxProbeDelay{16};

    void Probe(Clock::time_point now, Duration frame, Duration nominal) {
        if (++probe_frames <= ProbeSkip) {
            return;
        }
        probe_sum += frame;
        const int measured = probe_frames - ProbeSkip;
        const Duration mean = probe_sum / measured;
        if ((measured >= ProbeEarliestGiveUp && mean > nominal * GiveUpAbove) ||
            (measured == ProbeFrames && mean > nominal * KeepAtOrBelow)) {
            interval = 2;
            probing = false;
            probe_delay = (std::min)(
                std::chrono::duration_cast<Clock::duration>(probe_delay * 2),
                std::chrono::duration_cast<Clock::duration>(MaxProbeDelay));
            next_probe = now + probe_delay;
        } else if (measured == ProbeFrames) {
            probing = false;
            ResetWindow();
        }
    }

    void ResetWindow() {
        window.fill({});
        window_sum = {};
        window_next = window_count = 0;
    }

    u32 interval = 1;
    Clock::time_point last{};
    std::array<Duration, 60> window{};
    Duration window_sum{};
    size_t window_next = 0;
    size_t window_count = 0;
    bool probing = false;
    int probe_frames = 0;
    Duration probe_sum{};
    Clock::duration probe_delay = FirstProbeDelay;
    Clock::time_point next_probe{};
};

} // namespace Service::Nx2FrameInterval
