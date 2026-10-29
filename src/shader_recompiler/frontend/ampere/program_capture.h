// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <span>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
inline constexpr u32 MaximumCaptureInstructions = 65536;

struct CaptureIndirectBranchTable {
    u32 cbuf_index;
    u32 cbuf_offset;
};

inline std::optional<CaptureIndirectBranchTable> TrackCaptureIndirectBranchTable(
    std::span<const Instruction> code) {
    std::optional<CaptureIndirectBranchTable> result;
    for (u32 branch_index = 0; branch_index < code.size(); ++branch_index) {
        const auto& branch = code[branch_index];
        if (branch.Bits(0, 12) != 0x949)
            continue;
        if (branch.Bits(12, 4) != 7 || (branch.words[2] & ~0x3ffffU) != 0x03800000U ||
            s64{branch_index} * sizeof(Instruction) + sizeof(Instruction) +
                    RelativeOffset48(branch) !=
                0) {
            continue;
        }
        const u32 branch_source = branch.Bits(24, 8);
        std::optional<u32> load_index;
        for (u32 index = branch_index; index-- > 0;) {
            const auto& instruction = code[index];
            if (instruction.Bits(0, 12) == 0xb82 && instruction.Bits(12, 4) == 7 &&
                instruction.Bits(16, 8) == branch_source && instruction.words[1] == 0x00400000 &&
                instruction.words[2] == 0x00000800) {
                load_index = index;
                break;
            }
        }
        if (!load_index)
            continue;
        const u32 selector = code[*load_index].Bits(24, 8);
        std::optional<u32> shift_index;
        for (u32 index = *load_index; index-- > 0;) {
            const auto& instruction = code[index];
            if (instruction.Bits(0, 12) == 0x819 && instruction.Bits(12, 4) == 7 &&
                instruction.Bits(16, 8) == selector && instruction.words[1] == 2 &&
                instruction.words[2] == 0x000006ff) {
                shift_index = index;
                break;
            }
        }
        if (!shift_index)
            continue;
        const u32 bounded_selector = code[*shift_index].Bits(24, 8);
        bool bounded_to_three = false;
        for (u32 index = *shift_index; index-- > 0;) {
            const auto& instruction = code[index];
            if (instruction.Bits(0, 12) == 0x817 && instruction.Bits(12, 4) == 7 &&
                instruction.Bits(16, 8) == bounded_selector && instruction.words[1] == 2 &&
                instruction.words[2] == 0x03800000) {
                bounded_to_three = true;
                break;
            }
        }
        if (!bounded_to_three || result)
            return {};
        result = CaptureIndirectBranchTable{.cbuf_index = 1, .cbuf_offset = 0};
    }
    return result;
}

inline std::optional<std::array<u32, 3>> ValidateCaptureIndirectBranchTargets(
    const std::array<u32, 3>& targets, u32 source_base, u32 captured_instructions) {
    std::array<u32, 3> indices{};
    for (u32 i = 0; i < targets.size(); ++i) {
        if (targets[i] < source_base || (targets[i] - source_base) % sizeof(Instruction))
            return {};
        const u32 index = (targets[i] - source_base) / sizeof(Instruction);
        if (index >= MaximumCaptureInstructions || (i != 0 && targets[i] <= targets[i - 1])) {
            return {};
        }
        indices[i] = index;
    }
    if (indices[0] >= captured_instructions || indices[1] >= captured_instructions) {
        return {};
    }
    return indices;
}

inline bool CaptureIncludesIndirectBranchTargets(const std::array<u32, 3>& target_indices,
                                                 u32 captured_instructions) {
    return captured_instructions > target_indices.back();
}

class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = MaximumCaptureInstructions;

    Result Observe(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            const s64 relative =
                opcode == 0x945 ? RelativeOffset30(instruction) : RelativeOffset48(instruction);
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            if (opcode == 0x944 && instruction.Bits(12, 4) == 7) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
        }
        // A self-branch after the final EXIT is the exact mapped-code terminator.
        if (opcode == 0x947 && instruction.words[0] == 0x00007947 &&
            instruction.words[1] == 0xfffffff0 && instruction.words[2] == 0x0383ffff &&
            main_exited && index >= last_call_target && index >= last_forward_target) {
            return Result::Complete;
        }
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && instruction.Bits(87, 4) == 7 && !instruction.Bits(84, 2) && index >= last_forward_target) {
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
};
} // namespace Shader::Ampere
