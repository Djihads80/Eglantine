// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Optimization {

constexpr u32 MaskCanBeMinusOne{1u << 0};
constexpr u32 MaskCanBeZero{1u << 1};
constexpr u32 MaskCanBeOne{1u << 2};
constexpr u32 MaskUnknown{0};

constexpr u32 MaxMaskAnalysisDepth{8};

// negation maps -1 -> 1, 0 -> 0 and 1 -> -1: swap the -1 and 1 bits, keep 0
inline u32 NegateMask(u32 values) {
    return (values & MaskCanBeZero) | ((values & MaskCanBeMinusOne) << 2) |
           ((values & MaskCanBeOne) >> 2);
}

// x + 1 with x != 1: -1 becomes 0, 0 becomes 1
inline u32 AddOneToMask(u32 values) {
    return values << 1;
}

// x - 1 with x != -1: 0 becomes -1, 1 becomes 0
inline u32 SubtractOneFromMask(u32 values) {
    return values >> 1;
}

// mask constant: guest code encodes booleans as 0, 1 and -1 (all bits set)
inline bool IsMaskConstant(const IR::Value& candidate) {
    return candidate.IsImmediate() &&
           (candidate.U32() == 0 || candidate.U32() == 1 || candidate.U32() == 0xffffffffU);
}

/// "predicate mask" can take: -1, 0 and 1.
/// guest compilers build masks from selects, negations, +/-1 adjustments and phis.
/// everything unproven comes back as MaskUnknown, so callers only fold
/// with a proven mask and never fold on a wrong one.
inline u32 MaskValues(const IR::Value& value, u32 depth = 0) {
    const IR::Value resolved{value.Resolve()};
    if (resolved.IsImmediate()) {
        if (resolved.Type() != IR::Type::U32) {
            return MaskUnknown;
        }
        switch (resolved.U32()) {
        case 0xffffffffU:
            return MaskCanBeMinusOne;
        case 0:
            return MaskCanBeZero;
        case 1:
            return MaskCanBeOne;
        default:
            return MaskUnknown;
        }
    }
    if (depth >= MaxMaskAnalysisDepth) {
        return MaskUnknown;
    }
    const IR::Inst* const inst{resolved.InstRecursive()};
    switch (inst->GetOpcode()) {
    case IR::Opcode::SelectU32: {
        const u32 true_values{MaskValues(inst->Arg(1), depth + 1)};
        const u32 false_values{MaskValues(inst->Arg(2), depth + 1)};
        return true_values != MaskUnknown && false_values != MaskUnknown
                   ? true_values | false_values
                   : MaskUnknown;
    }
    case IR::Opcode::INeg32:
        return NegateMask(MaskValues(inst->Arg(0), depth + 1));
    case IR::Opcode::IAdd32:
        for (size_t index = 0; index < 2; ++index) {
            const IR::Value step{inst->Arg(1 - index).Resolve()};
            if (!step.IsImmediate()) {
                continue;
            }
            const u32 values{MaskValues(inst->Arg(index), depth + 1)};
            // +/- 1 stays in the domain only if the sum cannot escape it
            // 1 + 1 = 2 and -1 - 1 = -2 are not mask values
            if (step.U32() == 1 && (values & MaskCanBeOne) == 0) {
                return AddOneToMask(values);
            }
            if (step.U32() == 0xffffffffU && (values & MaskCanBeMinusOne) == 0) {
                return SubtractOneFromMask(values);
            }
            return MaskUnknown;
        }
        return MaskUnknown;
    case IR::Opcode::Phi: {
        u32 values{};
        for (size_t index = 0; index < inst->NumArgs(); ++index) {
            const u32 arg_values{MaskValues(inst->Arg(index), depth + 1)};
            if (arg_values == MaskUnknown) {
                return MaskUnknown;
            }
            values |= arg_values;
        }
        return values;
    }
    default:
        return MaskUnknown;
    }
}

} // namespace Shader::Optimization
