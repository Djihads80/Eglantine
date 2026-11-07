// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace InputCommon {
class InputSubsystem;
namespace TasInput {
class Tas;
}
} // namespace InputCommon

namespace Mcp {

/// One title from the frontend's game list.
struct GameEntry {
    std::string path;
    std::string title;
    u64 program_id = 0;
};

/// Raw presented frame, B G R A bytes per pixel, rows packed.
struct Frame {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> bgra;
};

/// Performance figures from the frontend. valid is false when it has none (e.g. no game).
struct PerfSnapshot {
    bool valid = false;
    double game_fps = 0.0;
    double emulation_speed = 0.0; ///< 1.0 = full speed
    double frametime_ms = 0.0;
    u32 shaders_building = 0;
};

/// Operations only the frontend can perform (it owns the window, the emulation thread and the
/// game list). Implementations must be callable from MCP worker threads and marshal to their UI
/// thread themselves. Defaults report "not supported" so frontends can grow incrementally.
class Host {
public:
    virtual ~Host() = default;

    /// Short frontend name (e.g. for the discovery file and status).
    [[nodiscard]] virtual std::string FrontendName() const = 0;

    /// Last stats the frontend computed (it owns the resetting perf counters).
    [[nodiscard]] virtual PerfSnapshot GetPerfSnapshot() {
        return {};
    }

    /// The frontend's input subsystem (virtual gamepad, TAS, touch...), or null.
    [[nodiscard]] virtual InputCommon::InputSubsystem* GetInputSubsystem() {
        return nullptr;
    }

    /// The frontend's game list, or nullopt if this frontend has none.
    [[nodiscard]] virtual std::optional<std::vector<GameEntry>> ListGames() {
        return std::nullopt;
    }

    /// Each returns an empty string on success or an error message.
    /// Boots the game at path, as if the user opened it.
    virtual std::string BootGame([[maybe_unused]] const std::string& path) {
        return "not supported by this frontend";
    }
    /// Pauses emulation.
    virtual std::string Pause() {
        return "not supported by this frontend";
    }
    /// Resumes paused emulation.
    virtual std::string Resume() {
        return "not supported by this frontend";
    }
    /// Stops the running game; the emulator stays open.
    virtual std::string Stop() {
        return "not supported by this frontend";
    }
    /// Closes the emulator (asynchronously: the reply is sent before the window closes).
    virtual std::string Quit() {
        return "not supported by this frontend";
    }

    /// Switches docked/handheld like the frontend's toggle (controller swap included), without
    /// dialogs. Returns an error or "".
    virtual std::string SetDocked([[maybe_unused]] bool docked) {
        return "not supported by this frontend";
    }

    /// Applies changed settings to the running game (System::ApplySettings on the thread the
    /// frontend uses for it). No-op when nothing runs.
    virtual std::string ApplySettings() {
        return "not supported by this frontend";
    }

    /// Runs fn with the TAS engine on the thread its controls belong to (its members are not
    /// synchronized; the frontend's own TAS hotkeys run there too), then refreshes the frontend's
    /// TAS display. With require_game, fails when no game is running. Returns an error or "".
    virtual std::string WithTas([[maybe_unused]] bool require_game,
                                [[maybe_unused]] std::function<void(InputCommon::TasInput::Tas&)> fn) {
        return "not supported by this frontend";
    }

    /// PNG bytes of the presented frame, or nullopt (error in `error`).
    virtual std::optional<std::vector<u8>> CaptureScreenshotPng(
        [[maybe_unused]] u32 max_width, [[maybe_unused]] std::chrono::milliseconds timeout,
        std::string& error) {
        error = "not supported by this frontend";
        return std::nullopt;
    }

    /// The presented frame as raw pixels at `width` (16:9), for server-side analysis.
    virtual std::optional<Frame> CaptureFrame([[maybe_unused]] u32 width,
                                              [[maybe_unused]] std::chrono::milliseconds timeout,
                                              std::string& error) {
        error = "not supported by this frontend";
        return std::nullopt;
    }
};

} // namespace Mcp
