// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCore {

// Counts the draws and clears the guest issues, so a presenter can tell whether the game is still
// rendering while it shows a blank picture.
class DrawCounter {
public:
    static DrawCounter& Get() {
        static DrawCounter counter;
        return counter;
    }

    void NoteDraw() {
        draws.fetch_add(1, std::memory_order_relaxed);
    }

    u64 Draws() const {
        return draws.load(std::memory_order_relaxed);
    }

    // Counts docked/handheld switches, so a presenter can time what follows one.
    void NoteModeSwitch() {
        mode_switches.fetch_add(1, std::memory_order_relaxed);
    }

    u64 ModeSwitches() const {
        return mode_switches.load(std::memory_order_relaxed);
    }

private:
    std::atomic<u64> draws{};
    std::atomic<u64> mode_switches{};
};

} // namespace VideoCore
