// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::Nx2Knob {

// Live switches for experiment variables, to A/B a change without a restart. The EDEN_NX2_*
// variable still sets the value at start; its debug knob bit (MCP debug_knobs, set / clear)
// inverts it while the game runs. Only switches read afresh at each use can be wired here.
enum : u8 {
    FrontBatch = 0,    ///< EDEN_NX2_FRONT_BATCH: batched 3D register writes on the split's front
    FramesAhead = 1,   ///< EDEN_NX2_FRAMES_AHEAD=1: the present waits for the previous frame
    FenceBatch = 2,    ///< EDEN_NX2_FENCE_BATCH: release fences in bursts
    InlineDefer = 3,   ///< EDEN_NX2_INLINE_DEFER: invalidate the caches one at a time
    MacroCopy = 4,     ///< EDEN_NX2_MACRO_COPY: always keep a macro's parameters in the vector
    FlushDraws32 = 5,  ///< EDEN_NX2_FLUSH_DRAWS: submit to the host GPU every 32 draws while set
    FlushDraws512 = 6, ///< ... every 512 draws while set (bit 5 wins)
    BankReupload = 7,  ///< EDEN_NX2_BANK_REUPLOAD: off while set (white flashes may come back)
    BackWait = 9,      ///< EDEN_NX2_BACK_WAIT=1: at a present wait for the GPU back thread to finish the
                       ///< frame's commands (and the previous fence), not for this frame's fence
    CondMacro = 8,     ///< EDEN_NX2_COND_MACRO (on by default, =0 starts it off): the back evaluates render-enable predicate macros; the knob inverts
};

/// `start_value` is what the environment chose; the knob inverts it.
[[nodiscard]] inline bool Live(bool start_value, u8 knob) {
    return start_value != Settings::GetDebugKnobAt(knob);
}

} // namespace VideoCommon::Nx2Knob
