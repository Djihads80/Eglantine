// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"

namespace Service::Nx2OotTime {

// Ocarina of Time's frame loop (main+0x6e6be0) computes each frame's delta as 60 / target fps and
// never looks at the clock, so everything runs in slow motion below that rate (and audio drifts
// from the video). The loader (nso.cpp) makes the loop load the delta from a word in the zeros
// after .text, and the present path (nx2_gsv.h) writes the measured frame time there.
inline constexpr u64 ProgramId = 0x0400486024DEC000ULL;
inline constexpr std::string_view BuildId = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
inline constexpr u64 DeltaOffset = 0x3C461E0;
inline constexpr u64 LoadDeltaOffset = 0x3C461E4;
inline constexpr u64 ReturnOffset = 0x3C461E8;
inline constexpr u64 HookOffset = 0x6E6CC4;
inline constexpr u32 HookInstruction = 0x14D57D48; // b 0x3c461e4

struct PatchWord {
    u64 module_offset;
    u32 original;
    u32 replacement;
};

/// Applied together or not at all: a branch into an unpatched cave would crash.
inline constexpr std::array<PatchWord, 4> PatchWords{{
    {DeltaOffset, 0x00000000, 0x3F800000},      // 1.0f
    {LoadDeltaOffset, 0x00000000, 0x1CFFFFE8},  // ldr s8, 0x3c461e0
    {ReturnOffset, 0x00000000, 0x172A82B8},     // b 0x6e6cc8
    {HookOffset, 0x1E201828, HookInstruction},  // fdiv s8, s1, s0 -> b 0x3c461e4
}};

/// EDEN_NX2_OOT_REAL_TIME=1 applies it. Off by default: it changes the game's speed, which scripted
/// test runs time their inputs by; off keeps the fixed 60 / target fps step (slow motion below it).
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_REAL_TIME");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

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
