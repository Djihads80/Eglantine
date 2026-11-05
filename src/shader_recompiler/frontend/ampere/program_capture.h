// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = 65536;

    /// Tracks control-flow targets while capturing instructions in program order.
    /// Returns Complete at a recognized program end or InvalidTarget for an invalid branch target.
    Result Observe(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            // BRA keeps its .DIV/.CONV mode in the offset's two low bits.
            const u32 low = instruction.words[1] & (opcode == 0x947 ? ~3U : ~0U);
            const u64 raw = low | (u64{instruction.words[2] & 0x3ffff} << 32);
            const s64 relative = opcode == 0x945 ? static_cast<s32>(instruction.words[1])
                                                : static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            // CALL.REL.NOINC (bit 86) pushes no return address: the compiler uses it as a jump
            // (the lowering treats it as one), so it never pairs with a RET.
            const bool noinc_call = opcode == 0x944 && (instruction.words[2] & 0x400000) != 0;
            if (opcode == 0x944 && !noinc_call) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
            // Every CALL but the lowering's predicated NOINC forward transfer (@P CALL.REL.NOINC
            // over the next BRA) is a linked call, whose callee ends at a RET.
            if (opcode == 0x944 && !(instruction.Bits(12, 4) != 7 && instruction.words[1] == 16 &&
                                     instruction.words[2] == 0x03c00000))
                has_calls = true;
            // The compiler ends every program with an unconditional branch to itself; with a jump
            // table (whose targets live in a constant buffer) that is the only reliable end. It
            // is also the end of a program without calls when no branch seen so far reaches past
            // it: programs whose only exits are predicated (TotK's cutscene video network loops
            // back with BRA and leaves through @P0 EXIT) have no unconditional EXIT to stop at.
            // As in the lowering, a BRA.DIV (mode 2) never branches and bits 87-90 add a condition.
            const u32 mode = instruction.Bits(32, 2);
            const bool always_branches =
                (mode == 3 || (mode == 0 && !instruction.Bits(91, 1))) && instruction.Bits(87, 4) == 7;
            if (opcode == 0x947 && instruction.Bits(12, 4) == 7 && always_branches &&
                target == s64{index} * 16 &&
                (computed_branch || (!has_calls && index >= last_forward_target)))
                return Result::Complete;
        }
        if (opcode == 0x949 || opcode == 0x958) computed_branch = true;
        if (computed_branch) return Result::Continue;
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && index >= last_forward_target) {
            if (!last_call_target) return Result::Complete;
            main_exited = true;
        }
        if (opcode == 0x950 && instruction.Bits(12, 4) == 7 && main_exited &&
            index >= last_call_target && index >= last_forward_target)
            return Result::Complete;
        return Result::Continue;
    }

private:
    u32 last_call_target{};
    u32 last_forward_target{};
    bool main_exited{};
    bool has_calls{};
    bool computed_branch{};
};
} // namespace Shader::Ampere
