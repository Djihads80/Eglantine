// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace Common {

/// The controller debug overlay, rendered into the presented frame itself so it is
/// visible inside gamescope/Steam: a Qt widget can never composite over the game
/// window. The frontend fills it (RGBA8, via QPainter) and the presentation path
/// alpha-blends it into the frame before the GPU copy.
struct ControllerOverlayFrame {
    std::mutex mutex;
    std::vector<u8> pixels; ///< RGBA8, width * height * 4
    u32 width = 0;
    u32 height = 0;
    bool enabled = false;
    /// Incremented on every push so presenters can cache by version
    u64 version = 0;
};

[[nodiscard]] inline ControllerOverlayFrame& GetControllerOverlayFrame() {
    static ControllerOverlayFrame instance;
    return instance;
}

} // namespace Common
