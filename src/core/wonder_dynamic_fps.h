// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>

#include <algorithm>
#include <cmath>
#include "common/common_types.h"

namespace Core {
class System;
bool TryHandleWonderTimingSvc(System& system, u32 imm, std::span<u64, 8> args);

// Experimental integer-Hz interface to Wonder's existing frame controller. One sample per
// present, not per query: the controller may be queried repeatedly while a frame is queued.
class WonderDynamicFps {
public:
    u32 Sample(u64 present, s64 now_ns, u32 target, u32 speed_percent,
               double applied_seconds = -1.0) {
        if (initialized && present == last_present)
            return rate;
        const s64 elapsed = now_ns - last_ns;
        const bool contiguous = initialized && present == last_present + 1 &&
                                target == last_target && speed_percent == last_speed;
        const u32 previous_rate = rate;
        initialized = true;
        last_present = present;
        last_ns = now_ns;
        last_target = target;
        last_speed = speed_percent;
        rate = target;
        const double nominal = 1e9 / target;
        if (!contiguous || speed_percent == 0 || elapsed <= 0 ||
            elapsed > std::max(250'000'000.0, nominal * 4) || !std::isfinite(applied_seconds) ||
            applied_seconds == 0) {
            phase_error = 0;
            return rate;
        }
        // Negative means no readback was supplied (standalone callers/tests). Runtime
        // supplies the actual preceding game step, independent of audit logging.
        if (applied_seconds < 0)
            applied_seconds = 1.0 / previous_rate;
        const double seconds = static_cast<double>(elapsed) * speed_percent / 100e9;
        phase_error = std::clamp(phase_error + seconds - applied_seconds, -0.050, 0.050);
        const u32 floor = std::min(target, 30U);
        // A slightly smaller step is needed to repay overspeed caused by pacing jitter.
        // Presentation still uses target; these extra simulation Hz do not add frames.
        const u32 ceiling = std::min(1000U, (target * 5 + 3) / 4);
        const double step = std::clamp(seconds + 0.20 * phase_error, 1.0 / ceiling, 1.0 / floor);
        const double hz = 1.0 / step;
        const u32 lower = std::clamp(static_cast<u32>(std::floor(hz)), floor, ceiling);
        const u32 upper = std::min(lower + 1, ceiling);
        rate = std::abs(1.0 / lower - step) <= std::abs(1.0 / upper - step) ? lower : upper;
        return rate;
    }

private:
    bool initialized{};
    u64 last_present{};
    s64 last_ns{};
    u32 rate{};
    u32 last_target{}, last_speed{};
    double phase_error{};
};
// Readback audit: scale is read from the game's shared simulation-time field, not
// inferred from the rate requested by Sample(). It describes the preceding frame.
class WonderSpeedAudit {
public:
    bool Observe(u64 present, s64 now_ns, float scale) {
        if (initialized && present == last_present)
            return false;
        const s64 elapsed = now_ns - last_ns;
        const bool covered = initialized && present == last_present + 1 && elapsed > 0 &&
                             elapsed <= 250'000'000 && std::isfinite(scale) && scale > 0 &&
                             scale <= 60;
        if (covered) {
            wall_seconds += elapsed / 1e9;
            game_seconds += scale / 60.0;
            ++frames;
            if (elapsed <= 33'333'333) {
                above30_wall_seconds += elapsed / 1e9;
                above30_game_seconds += scale / 60.0;
                above30_run_seconds += elapsed / 1e9;
                // Exclude the first two seconds after a dip/reset: rate feedback is
                // delayed by one frame and may still be correcting the prior stall.
                if (above30_run_seconds > 2.0) {
                    steady30_wall_seconds += elapsed / 1e9;
                    steady30_game_seconds += scale / 60.0;
                }
            } else {
                below30_wall_seconds += elapsed / 1e9;
                below30_game_seconds += scale / 60.0;
                above30_run_seconds = 0;
            }
        } else if (initialized && elapsed > 0) {
            uncovered_seconds += elapsed / 1e9;
            ++discontinuities;
            above30_run_seconds = 0;
        }
        initialized = true;
        last_ns = now_ns;
        last_present = present;
        if (wall_seconds + uncovered_seconds - last_report >= 5) {
            last_report = wall_seconds + uncovered_seconds;
            return true;
        }
        return false;
    }
    double wall_seconds{}, game_seconds{}, uncovered_seconds{};
    u64 frames{}, discontinuities{};
    double above30_wall_seconds{}, above30_game_seconds{};
    double below30_wall_seconds{}, below30_game_seconds{};
    double steady30_wall_seconds{}, steady30_game_seconds{};

private:
    bool initialized{};
    u64 last_present{};
    s64 last_ns{};
    double last_report{}, above30_run_seconds{};
};
} // namespace Core
