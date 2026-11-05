// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

// TEMPORARY opt-in visual workaround for Ocarina of Time, not an emulator fix.
inline std::optional<size_t> ApplyOotVisualWorkaround(IR::Program& program, u64 title_id,
                                                     u64 shader_hash, bool enabled) {
    if (!enabled || title_id != 0x0400486024DEC000ULL) {
        return std::nullopt;
    }
    const bool fog = shader_hash == 0x780c0536b89fc89eULL && program.stage == Stage::Compute;
    const bool exposure = shader_hash == 0xbded5ce917183214ULL && program.stage == Stage::Fragment;
    if (!fog && !exposure) {
        return std::nullopt;
    }
    const auto has_opcode = [](const IR::Value& value, IR::Opcode opcode) {
        return !value.IsImmediate() && value.InstRecursive()->GetOpcode() == opcode;
    };
    const auto negative_exp = [&](const IR::Value& value) {
        if (has_opcode(value, IR::Opcode::FPNeg32)) {
            return has_opcode(value.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
        }
        if (!has_opcode(value, IR::Opcode::BitCastF32U32)) {
            return false;
        }
        const auto bits = value.InstRecursive()->Arg(0);
        if (!has_opcode(bits, IR::Opcode::BitwiseXor32)) {
            return false;
        }
        const auto* flip = bits.InstRecursive();
        const auto source = flip->Arg(0);
        return flip->Arg(1).IsImmediate() && flip->Arg(1).U32() == 0x80000000U &&
               has_opcode(source, IR::Opcode::BitCastU32F32) &&
               has_opcode(source.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
    };
    std::vector<std::pair<IR::Block*, IR::Inst*>> matches;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (fog && inst.GetOpcode() == IR::Opcode::FPAdd32 &&
                ((negative_exp(inst.Arg(0)) && has_opcode(inst.Arg(1), IR::Opcode::FPExp2)) ||
                 (negative_exp(inst.Arg(1)) && has_opcode(inst.Arg(0), IR::Opcode::FPExp2)))) {
                matches.emplace_back(block, &inst);
            }
            if (exposure && inst.GetOpcode() == IR::Opcode::GetCbufF32 &&
                inst.Arg(0).IsImmediate() && inst.Arg(0).U32() == 3 &&
                inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == 0x38) {
                matches.emplace_back(block, &inst);
            }
        }
    }
    if ((fog && matches.size() != 1) || (exposure && (matches.empty() || matches.size() > 3))) {
        return 0;
    }
    for (const auto& [block, inst] : matches) {
        const auto insertion = block->Instructions().iterator_to(*inst);
        const auto original = block->PrependNewInst(insertion, *inst);
        IR::IREmitter ir{*block, insertion};
        const auto scaled = ir.FPMul(IR::F32{IR::Value{&*original}},
                                     ir.Imm32(fog ? 0.25f : 0.65f));
        inst->ReplaceUsesWith(scaled);
    }
    return matches.size();
}

} // namespace Shader::Optimization
