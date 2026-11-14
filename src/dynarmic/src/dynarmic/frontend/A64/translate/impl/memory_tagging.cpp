// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ARMv8.5 memory tagging, with tags ignored: every allocation tag is 0, tag stores only do
// their address writeback, and the STZG forms still zero the granules they cover.

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

static IR::U64 TagBase(TranslatorVisitor& v, Reg Rn) {
    return Rn == Reg::SP ? IR::U64{v.SP(64)} : IR::U64{v.X(64, Rn)};
}

static void TagSetBase(TranslatorVisitor& v, Reg Rn, IR::U64 value) {
    if (Rn == Reg::SP) {
        v.SP(64, value);
    } else {
        v.X(64, Rn, value);
    }
}

bool TranslatorVisitor::STG_family(Imm<2> opc, Imm<9> imm9, Imm<2> mode, Reg Rn, Reg Rt) {
    const size_t opcv = opc.ZeroExtend<size_t>();
    const size_t modev = mode.ZeroExtend<size_t>();
    if (modev == 0b00) {
        // LDG (opc 01) leaves Xt as-is since tags are 0; STZGM/STGM/LDGM are EL1-only.
        if (opcv == 0b01) {
            return true;
        }
        return UnallocatedEncoding();
    }

    const s64 offset = imm9.SignExtend<s64>() << 4;
    const IR::U64 base = TagBase(*this, Rn);
    const IR::U64 offset_address = ir.Add(base, ir.Imm64(static_cast<u64>(offset)));
    const IR::U64 address = modev == 0b01 ? base : offset_address;  // 01 = post-index

    const bool zero = (opcv & 1) != 0;          // STZG, STZ2G
    const size_t granules = (opcv & 2) ? 2 : 1;  // ST2G, STZ2G
    if (zero) {
        const IR::U64 zero64 = ir.Imm64(0);
        for (size_t i = 0; i < granules * 2; ++i) {
            Mem(ir.Add(address, ir.Imm64(i * 8)), 8, IR::AccType::NORMAL, zero64);
        }
    }
    (void)Rt;
    if (modev != 0b10) {
        TagSetBase(*this, Rn, offset_address);
    }
    return true;
}

bool TranslatorVisitor::STGP(Imm<2> mode, Imm<7> imm7, Reg Rt2, Reg Rn, Reg Rt) {
    const size_t modev = mode.ZeroExtend<size_t>();
    if (modev == 0b00) {
        return UnallocatedEncoding();
    }
    const s64 offset = imm7.SignExtend<s64>() << 4;
    const IR::U64 base = TagBase(*this, Rn);
    const IR::U64 offset_address = ir.Add(base, ir.Imm64(static_cast<u64>(offset)));
    const IR::U64 address = modev == 0b01 ? base : offset_address;
    Mem(address, 8, IR::AccType::NORMAL, X(64, Rt));
    Mem(ir.Add(address, ir.Imm64(8)), 8, IR::AccType::NORMAL, X(64, Rt2));
    if (modev != 0b10) {
        TagSetBase(*this, Rn, offset_address);
    }
    return true;
}

bool TranslatorVisitor::IRG(Reg Rm, Reg Rn, Reg Rd) {
    (void)Rm;
    TagSetBase(*this, Rd, TagBase(*this, Rn));
    return true;
}

bool TranslatorVisitor::GMI(Reg Rm, Reg Rn, Reg Rd) {
    (void)Rn;
    // Tag of Xn is always 0, so the exclusion mask gains bit 0.
    X(64, Rd, ir.Or(X(64, Rm), ir.Imm64(1)));
    return true;
}

bool TranslatorVisitor::ADDG(Imm<6> offset_imm, Imm<4> tag_offset, Reg Rn, Reg Rd) {
    (void)tag_offset;
    const u64 offset = offset_imm.ZeroExtend<u64>() << 4;
    TagSetBase(*this, Rd, ir.Add(TagBase(*this, Rn), ir.Imm64(offset)));
    return true;
}

bool TranslatorVisitor::SUBG(Imm<6> offset_imm, Imm<4> tag_offset, Reg Rn, Reg Rd) {
    (void)tag_offset;
    const u64 offset = offset_imm.ZeroExtend<u64>() << 4;
    TagSetBase(*this, Rd, ir.Sub(TagBase(*this, Rn), ir.Imm64(offset)));
    return true;
}

}  // namespace Dynarmic::A64
