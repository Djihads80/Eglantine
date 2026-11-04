// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore {

/// What draws a game's own DLSS evaluation (Fire Emblem: Fortune's Weave's network) this session.
enum class GameDlssState : u8 {
    None,        ///< No DLSS evaluation seen yet.
    Checking,    ///< The network runs and is being checked; the replacement upscale is shown.
    Network,     ///< The game's own network is shown.
    Replacement, ///< The replacement upscale is shown; the reason says why.
};

/// Why the replacement upscale draws instead of the game's own network.
enum class GameDlssReason : u8 {
    None,
    TurnedOff,         ///< The "Use the game's own DLSS" setting is off.
    NvidiaDlss,        ///< NVIDIA DLSS is turned on and runs in its place.
    ResolutionScaling, ///< The network only makes the game's own resolution.
    Unsupported,       ///< Not Vulkan, or compute shaders can't be made 32 threads wide.
    PassesCannotRun,   ///< Some of the network's programs can't be translated (rejected_passes).
    InvalidOutput,     ///< The network failed its checks: NaN or infinite output.
    OutputMismatch,    ///< The network failed its checks: output not matching its input.
};

struct GameDlssStatus {
    GameDlssState state{};
    GameDlssReason reason{};
    u32 rejected_passes{};
};

/// For the Graphics tab; safe to call from any thread.
[[nodiscard]] GameDlssStatus GetGameDlssStatus();
void SetGameDlssStatus(const GameDlssStatus& status);

/// The reason in English, for the log.
[[nodiscard]] const char* GameDlssReasonText(GameDlssReason reason);

/// One health check of a game's DLSS network output against its input, by blocks of a coarse grid
/// (host_shaders/dlss_network_probe.comp).
struct GameDlssProbe {
    u32 blocks{};          ///< Blocks compared.
    u32 mismatched{};      ///< Blocks whose output brightness is off the input's by more than half.
    u32 invalid_samples{}; ///< NaN or infinite output samples.
    f32 worst{};           ///< The largest relative brightness difference of a block.
};

} // namespace VideoCore
