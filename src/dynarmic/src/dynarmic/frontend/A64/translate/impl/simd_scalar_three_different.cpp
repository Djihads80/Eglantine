// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {
namespace {
enum class ScalarDoublingMultiplyLongBehavior {
    None,
    Accumulate,
    Subtract,
};

bool ScalarSaturatingDoublingMultiplyLong(TranslatorVisitor& v, Imm<2> size, Vec Vm, Vec Vn, Vec Vd, ScalarDoublingMultiplyLongBehavior behavior) {
    if (size == 0b00 || size == 0b11) {
        return v.ReservedValue();
    }

    const size_t esize = 8 << size.ZeroExtend();

    const IR::U128 operand1 = v.ir.ZeroExtendToQuad(v.ir.VectorGetElement(esize, v.V(128, Vn), 0));
    const IR::U128 operand2 = v.ir.ZeroExtendToQuad(v.ir.VectorGetElement(esize, v.V(128, Vm), 0));
    const IR::U128 product = v.ir.VectorSignedSaturatedDoublingMultiplyLong(esize, operand1, operand2);
    const IR::U128 result = [&] {
        if (behavior == ScalarDoublingMultiplyLongBehavior::None) {
            return product;
        }

        const IR::U128 accumulator = v.ir.ZeroExtendToQuad(v.ir.VectorGetElement(2 * esize, v.V(128, Vd), 0));
        if (behavior == ScalarDoublingMultiplyLongBehavior::Accumulate) {
            return v.ir.VectorSignedSaturatedAdd(2 * esize, accumulator, product);
        }
        return v.ir.VectorSignedSaturatedSub(2 * esize, accumulator, product);
    }();

    v.V(128, Vd, result);
    return true;
}
}  // Anonymous namespace

bool TranslatorVisitor::SQDMLAL_vec_1(Imm<2> size, Vec Vm, Vec Vn, Vec Vd) {
    return ScalarSaturatingDoublingMultiplyLong(*this, size, Vm, Vn, Vd, ScalarDoublingMultiplyLongBehavior::Accumulate);
}

bool TranslatorVisitor::SQDMLSL_vec_1(Imm<2> size, Vec Vm, Vec Vn, Vec Vd) {
    return ScalarSaturatingDoublingMultiplyLong(*this, size, Vm, Vn, Vd, ScalarDoublingMultiplyLongBehavior::Subtract);
}

bool TranslatorVisitor::SQDMULL_vec_1(Imm<2> size, Vec Vm, Vec Vn, Vec Vd) {
    return ScalarSaturatingDoublingMultiplyLong(*this, size, Vm, Vn, Vd, ScalarDoublingMultiplyLongBehavior::None);
}

}  // namespace Dynarmic::A64
