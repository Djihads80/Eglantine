// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Common {

/// The state shared between the console-mode engine (in the main window, ticking at
/// 250ms) and its settings UI (the Players tab in Controls): which pads are live, which
/// one is the handheld's own, the assigned players, the "identify" handshake and swap
/// requests. The UI reads and declares; the engine observes and obeys.
struct ControllerConsoleState {
    std::mutex mutex;
    /// Live pads as (display name, identity) in engine order
    std::vector<std::pair<std::string, std::string>> pads;
    /// Set by the Identify button: capture the next pad that has a button pressed
    bool identify_armed = false;
    std::chrono::steady_clock::time_point identify_armed_at{};

    /// One assigned player, as the Players tab lists it
    struct PlayerRow {
        int player; ///< 0-based player index
        std::string name;
        bool active;  ///< the pad has an input held (or was just tapped)
        bool leaving; ///< the controller left and nothing took its place: shown red, then gone
        int flash;    ///< 0 none, 1 green (a controller took the player), 2 red (one left)
        long long flash_since; ///< ms since epoch; the flash blinks for 1.5 seconds from here
    };
    /// True while an automatic assignment mode runs (the engine owns the players)
    bool engine_active = false;
    /// The players the engine assigned, in player order
    std::vector<PlayerRow> players;
    /// The handheld's own pad: its name ("" when not identified) and whether it holds a player
    std::string handheld_name;
    bool handheld_has_player = false;
    /// Swaps requested by the Players tab, as (player a, player b); the engine applies them
    /// on its next tick, for this session only (the saved controller setup is untouched)
    std::vector<std::pair<int, int>> swap_requests;
    /// Players whose controller the Controllers tab asked to rumble (a click on its row)
    std::vector<int> rumble_requests;
    /// Bumped whenever the engine reloads the emulated controllers (a swap, a pad arriving or
    /// leaving): the Controls page refreshes its player tabs from them when it changes
    unsigned long long generation = 0;
};

[[nodiscard]] inline ControllerConsoleState& GetControllerConsoleState() {
    static ControllerConsoleState instance;
    return instance;
}

} // namespace Common
