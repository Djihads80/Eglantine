// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::Nx2Knob {

// Live switches for experiment variables, to A/B a change without a restart. The EDEN_NX2_*
// variable still sets the value at start; its debug knob bit (MCP debug_knobs, set / clear)
// inverts it while the game runs. Only switches read afresh at each use can be wired here.
// Bits 0-2, 4, 7 and 8 are free: their switches were removed.
enum : u8 {
    InlineDefer = 3,   ///< EDEN_NX2_INLINE_DEFER: invalidate the caches one at a time
    FlushDraws32 = 5,  ///< EDEN_NX2_FLUSH_DRAWS: submit to the host GPU every 32 draws while set
    FlushDraws512 = 6, ///< ... every 512 draws while set (bit 5 wins)
    BackWait = 9,      ///< EDEN_NX2_BACK_WAIT=1: at a present wait for the GPU back thread to finish the
                       ///< frame's commands (and the previous fence), not for this frame's fence
    HostImport = 10,   ///< EDEN_NX2_HOST_IMPORT=1: 64 KiB native uniform banks read the guest's pages in place (no copy)
};

/// `start_value` is what the environment chose; the knob inverts it.
[[nodiscard]] inline bool Live(bool start_value, u8 knob) {
    return start_value != Settings::GetDebugKnobAt(knob);
}

} // namespace VideoCommon::Nx2Knob
