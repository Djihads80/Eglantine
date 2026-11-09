// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>

#include "common/common_types.h"

namespace Service::Nx2OotTime {

// Ocarina of Time's frame loop (main+0x6e6be0) computes each frame's delta as 60 / target fps and
// never looks at the clock, so everything runs in slow motion below that rate. The built-in patch
// in patch_manager.cpp makes the loop load the delta from a word in the zeros after .text, and the
// present path writes the measured frame time there.
inline constexpr u64 ProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 DeltaOffset = 0x3C461E0;
inline constexpr u64 LoadDeltaOffset = 0x3C461E4;
inline constexpr u64 ReturnOffset = 0x3C461E8;
inline constexpr u64 HookOffset = 0x6E6CC4;
inline constexpr u32 HookInstruction = 0x14D57D48; // b 0x3c461e4

/// The game's delta (1.0 = one 60th of a second) for the frame that follows a present.
class FrameDelta {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] float Update(Clock::time_point now) {
        if (last != Clock::time_point{}) {
            // A pause or a shader stall isn't time the game should catch up on.
            const float seconds = std::chrono::duration<float>(now - last).count();
            average_seconds += ((std::min)(seconds, MaxFrameSeconds) - average_seconds) * Smoothing;
        }
        last = now;
        const float delta = std::clamp(average_seconds * 60.0f, 1.0f, MaxDelta);
        // Near 60 fps, keep the game's own fixed step instead of following timing jitter.
        return delta < SnapBelow ? 1.0f : delta;
    }

private:
    static constexpr float MaxFrameSeconds = 0.1f;
    static constexpr float Smoothing = 0.125f;
    static constexpr float MaxDelta = 4.0f;
    static constexpr float SnapBelow = 1.1f;

    Clock::time_point last{};
    float average_seconds = 1.0f / 60.0f;
};

} // namespace Service::Nx2OotTime
