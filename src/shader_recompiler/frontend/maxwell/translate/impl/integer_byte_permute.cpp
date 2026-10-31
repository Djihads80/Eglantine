// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
// PRMT in its default mode: each result byte is picked by a selector nibble from the eight bytes
// {c, a} (0-3 from a, 4-7 from c); bit 3 of the nibble replicates the picked byte's sign bit.
void PRMT(TranslatorVisitor& v, u64 insn, const IR::U32& selector, const IR::U32& src_c) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_a_reg;
        BitField<48, 3, u64> mode;
    } const prmt{insn};
    if (prmt.mode != 0) {
        throw NotImplementedException("PRMT mode {}", prmt.mode.Value());
    }
    const IR::U32 src_a{v.X(prmt.src_a_reg)};
    IR::U32 result{v.ir.Imm32(0)};
    for (u32 byte = 0; byte < 4; ++byte) {
        const IR::U32 nibble{v.ir.BitFieldExtract(selector, v.ir.Imm32(byte * 4), v.ir.Imm32(4))};
        const IR::U32 index{v.ir.BitwiseAnd(nibble, v.ir.Imm32(7))};
        const IR::U32 source{
            IR::U32{v.ir.Select(v.ir.ILessThan(index, v.ir.Imm32(4), false), src_a, src_c)}};
        const IR::U32 offset{v.ir.ShiftLeftLogical(v.ir.BitwiseAnd(index, v.ir.Imm32(3)), v.ir.Imm32(3))};
        const IR::U32 value{v.ir.BitFieldExtract(source, offset, v.ir.Imm32(8))};
        const IR::U32 sign{v.ir.BitFieldExtract(source, v.ir.IAdd(offset, v.ir.Imm32(7)), v.ir.Imm32(1), true)};
        const IR::U1 replicate{v.ir.INotEqual(v.ir.BitwiseAnd(nibble, v.ir.Imm32(8)), v.ir.Imm32(0))};
        const IR::U32 picked{IR::U32{v.ir.Select(replicate, sign, value)}};
        result = v.ir.BitFieldInsert(result, picked, v.ir.Imm32(byte * 8), v.ir.Imm32(8));
    }
    v.X(prmt.dest_reg, result);
}
} // Anonymous namespace

void TranslatorVisitor::PRMT_reg(u64 insn) {
    PRMT(*this, insn, GetReg20(insn), GetReg39(insn));
}

void TranslatorVisitor::PRMT_rc(u64 insn) {
    PRMT(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::PRMT_cr(u64 insn) {
    PRMT(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::PRMT_imm(u64 insn) {
    PRMT(*this, insn, GetImm20(insn), GetReg39(insn));
}

} // namespace Shader::Maxwell
