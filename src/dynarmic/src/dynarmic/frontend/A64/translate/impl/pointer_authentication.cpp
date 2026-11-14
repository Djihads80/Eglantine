// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ARMv8.3 pointer authentication, implemented as the identity: PAC* never adds a code and
// AUT*/XPAC* have nothing to strip, so signed pointers are just plain pointers. The HINT-space
// forms (PACIASP, AUTIBSP, ...) already decode as HINT/NOP.

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

bool TranslatorVisitor::PACAUT_reg(bool Z, Imm<3> op, Reg Rn, Reg Rd) {
    (void)op;
    (void)Rd;
    if (Z && Rn != Reg::R31) {
        return UnallocatedEncoding();
    }
    return true;
}

bool TranslatorVisitor::XPAC_1(bool D, Reg Rd) {
    (void)D;
    (void)Rd;
    return true;
}

bool TranslatorVisitor::PACGA(Reg Rm, Reg Rn, Reg Rd) {
    // Any deterministic value works, since the code is only ever compared to a recomputation.
    const IR::U64 m = Rm == Reg::SP ? IR::U64{SP(64)} : IR::U64{X(64, Rm)};
    X(64, Rd, ir.And(ir.Eor(X(64, Rn), m), ir.Imm64(0xFFFFFFFF00000000ULL)));
    return true;
}

bool TranslatorVisitor::RETA(bool M) {
    (void)M;
    ir.SetPC(X(64, Reg::R30));
    ir.SetTerm(IR::Term::PopRSBHint{});
    return false;
}

bool TranslatorVisitor::BRA_BLRA(bool Z, bool L, bool M, Reg Rn, Reg Rm) {
    (void)M;
    if (!Z && Rm != Reg::R31) {
        return UnallocatedEncoding();
    }
    const auto target = X(64, Rn);
    if (L) {
        X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
        ir.PushRSB(ir.current_location->AdvancePC(4));
    }
    ir.SetPC(target);
    ir.SetTerm(IR::Term::FastDispatchHint{});
    return false;
}

bool TranslatorVisitor::LDRA(bool M, bool S, Imm<9> imm9, bool W, Reg Rn, Reg Rt) {
    (void)M;
    const s64 offset = concatenate(Imm<1>{S ? 1u : 0u}, imm9).SignExtend<s64>() << 3;
    const IR::U64 base = Rn == Reg::SP ? IR::U64{SP(64)} : IR::U64{X(64, Rn)};
    const IR::U64 address = ir.Add(base, ir.Imm64(static_cast<u64>(offset)));
    const IR::U64 data = Mem(address, 8, IR::AccType::NORMAL);
    if (W) {
        if (Rn == Reg::SP) {
            SP(64, address);
        } else {
            X(64, Rn, address);
        }
    }
    X(64, Rt, data);
    return true;
}

}  // namespace Dynarmic::A64
