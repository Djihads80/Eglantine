// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {

// Not a Maxwell instruction: the Ampere lowering emits it for HMMA.1688/.16816.F16 when the
// compact software form is asked for (bit 48). Bit 49 selects K = 8 instead of K = 16.
void TranslatorVisitor::TENSOR_MUL_ADD(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> matrix_a;
        BitField<20, 8, IR::Reg> matrix_b;
        BitField<39, 8, IR::Reg> matrix_c;
        BitField<48, 1, u64> software;
        BitField<49, 1, u64> short_depth;
    } const tensor{insn};

    if (tensor.software == 0) {
        throw NotImplementedException("Unsupported matrix multiply encoding");
    }
    const IR::Reg ar{tensor.matrix_a};
    const IR::Reg br{tensor.matrix_b};
    const IR::Reg cr{tensor.matrix_c};
    const IR::Reg dr{tensor.dest_reg};
    const bool short_depth{tensor.short_depth != 0};
    const bool has_c{cr != IR::Reg::RZ};

    const IR::U32 a0{X(ar)};
    const IR::U32 a1{X(ar + 1)};
    const IR::U32 a2{short_depth ? ir.Imm32(0u) : X(ar + 2)};
    const IR::U32 a3{short_depth ? ir.Imm32(0u) : X(ar + 3)};
    const IR::U32 b0{X(br)};
    const IR::U32 b1{short_depth ? ir.Imm32(0u) : X(br + 1)};
    const IR::U32 c0{has_c ? X(cr) : ir.Imm32(0u)};
    const IR::U32 c1{has_c ? X(cr + 1) : ir.Imm32(0u)};

    const IR::Value result{ir.TensorMulAdd(ir.CompositeConstruct(a0, a1, a2, a3),
                                           ir.CompositeConstruct(b0, b1),
                                           ir.CompositeConstruct(c0, c1), short_depth ? 8U : 16U)};
    X(dr, IR::U32{ir.CompositeExtract(result, 0)});
    X(dr + 1, IR::U32{ir.CompositeExtract(result, 1)});
}

} // namespace Shader::Maxwell
