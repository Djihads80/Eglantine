// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Executes the unpredicated integer subset the IMAD.HI/WIDE lowering emits, following the
// bit layouts in Eglantine's Maxwell translators (XMAD, IADD, ISCADD, SHR, MOV).
struct Machine {
    std::array<u32, 256> regs{};
    std::array<std::array<u32, 64>, 18> cbufs{};
    bool carry = false;
    std::array<bool, 8> preds{false, false, false, false, false, false, false, true};
    u32 Reg(u64 word, u32 bit) const {
        const u32 index = static_cast<u32>((word >> bit) & 255);
        return index == 255 ? 0U : regs[index];
    }
    u32 Cbuf(u64 word) const {
        return cbufs[(word >> 34) & 31][(word >> 20) & 0x3fff];
    }
    void Run(u64 word) {
        const u32 dest = static_cast<u32>(word & 255);
        const auto write = [&](u32 value) { if (dest != 255) regs[dest] = value; };
        const u64 top = word >> 52;
        REQUIRE(((word >> 16) & 15) == 7); // Unpredicated.
        if ((word >> 56) == 0x01) return write(static_cast<u32>(word >> 20)); // MOV32I
        if (top == 0x5ca) { // SEL (reg)
            const bool pred = preds[(word >> 39) & 7], negated = (word >> 42) & 1;
            const u32 a = Reg(word, 8), b = Reg(word, 20);
            return write(pred ? (negated ? b : a) : (negated ? a : b));
        }
        if (top == 0x38a) { // SEL (imm): pred ? Ra : imm, swapped by the negation bit
            REQUIRE(((word >> 56) & 1) == 0);
            const bool pred = preds[(word >> 39) & 7];
            const u32 a = Reg(word, 8), b = static_cast<u32>((word >> 20) & 0x7ffff);
            const bool negated = (word >> 42) & 1;
            return write(pred ? (negated ? b : a) : (negated ? a : b));
        }
        if (top == 0x4c9) return write(Cbuf(word));
        if (top == 0x5c9) return write(Reg(word, 20));
        if (top == 0x382) { // SHR (imm)
            const u32 a = Reg(word, 8), shift = static_cast<u32>((word >> 20) & 31);
            REQUIRE(((word >> 48) & 1) == 0);
            return write(a >> shift);
        }
        if (top == 0x5c1) { // IADD (reg); bit 52 selects ISCADD
            const u32 a = Reg(word, 8), b = Reg(word, 20);
            if ((word >> 51) & 1) { // ISCADD: bit 48 negates b; neg_a (with PO) is not emitted.
                REQUIRE(((word >> 49) & 1) == 0);
                return write((a << ((word >> 39) & 31)) + (((word >> 48) & 1) ? 0U - b : b));
            }
            REQUIRE(((word >> 48) & 3) == 0);
            const u64 sum = u64{a} + b + (((word >> 43) & 1) && carry ? 1 : 0);
            if ((word >> 47) & 1) carry = (sum >> 32) != 0;
            return write(static_cast<u32>(sum));
        }
        if ((word >> 56) == 0x5b || (word >> 57) == 0x1b) { // XMAD (reg) and XMAD (imm)
            const bool immediate = (word >> 56) == 0x36;
            const u32 a = Reg(word, 8), c = Reg(word, 39);
            const u32 b = immediate ? static_cast<u32>((word >> 20) & 0xffff) : Reg(word, 20);
            REQUIRE(((word >> 36) & 7) == 0); // No PSL, MRG or X.
            REQUIRE(((word >> 47) & 7) == 0); // No CC, signed halves.
            const u32 a_half = (word >> 53) & 1 ? a >> 16 : a & 0xffff;
            const u32 b_half = !immediate && ((word >> 35) & 1) ? b >> 16 : b & 0xffff;
            const u32 mode = static_cast<u32>((word >> 50) & 7);
            const u32 addend = mode == 0 ? c : mode == 1 ? (c & 0xffff) : mode == 2 ? (c >> 16)
                             : mode == 4 ? (b << 16) + c : 0xdeadbeef;
            REQUIRE(mode != 3);
            REQUIRE(mode <= 4);
            return write(a_half * b_half + addend);
        }
        FAIL("unmodelled Maxwell instruction " << std::hex << word);
    }
};
} // Anonymous namespace

TEST_CASE("Ampere IMAD.HI.U32 with a constant-buffer pair adds the 64-bit addend", "[video_core][ampere]") {
    // ptxas SM86 emits this for hi32(u64(a) * b + u64 parameter): IMAD.HI.U32 R5, R5, R0, c[0x0][0x170].
    constexpr std::array<std::array<u32, 4>, 8> samples{{
        {0, 0, 0, 0},
        {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
        {0xffffffff, 0xffffffff, 0x00000001, 0},
        {0x80000000, 0x00000002, 0x00000000, 0x7fffffff},
        {0x12345678, 0x9abcdef0, 0xfedcba98, 0x01234567},
        {0x0000ffff, 0x00010001, 0xffff0001, 0},
        {0xdeadbeef, 0xcafef00d, 0x80000000, 0xfffffffe},
        {0x00000001, 0x00000001, 0xffffffff, 0x00000005},
    }};
    for (const u32 bank : {0U, 1U}) {
        const u32 offset = 0x170;
        const u32 rd = 44, ra = 64, rb = 39;
        const Instruction native{{0x00007627U | (rd << 16) | (ra << 24), (bank << 22) | (offset << 6),
                                  0x078e0000U | rb, 0x000fe200}};
        const auto result = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(result));
        for (const auto& sample : samples) {
            Machine machine;
            for (u32 i = 0; i < 255; ++i) machine.regs[i] = 0x5a5a0000U + i;
            machine.regs[ra] = sample[0];
            machine.regs[rb] = sample[1];
            machine.cbufs[bank][offset / 4] = sample[2];
            machine.cbufs[bank][offset / 4 + 1] = sample[3];
            for (const u64 word : result.instructions) {
                if ((word >> 52) == 0xe30) break; // EXIT
                machine.Run(word);
            }
            const u64 addend = u64{sample[2]} | (u64{sample[3]} << 32);
            const u64 expected = (u64{sample[0]} * sample[1] + addend) >> 32;
            CHECK(machine.regs[rd] == static_cast<u32>(expected));
            CHECK(machine.regs[ra] == sample[0]);
            CHECK(machine.regs[rb] == sample[1]);
        }
    }
}

TEST_CASE("Ampere IMAD.HI with a constant-buffer pair rejects unproven forms", "[video_core][ampere]") {
    const Instruction native{{0x402c7627, 0x00400000, 0x078e0027, 0x040fe200}};
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})));
    Instruction last_word = native;
    last_word.words[1] = 0x00400000U | (0xfffcU << 6); // The pair would leave the 64 KiB bank.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{last_word, Exit})));
    Instruction signed_carry = native;
    signed_carry.words[2] = (signed_carry.words[2] & ~(7U << 17)) | (1U << 9) | (1U << 17);
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{signed_carry, Exit})));
}

TEST_CASE("Ampere IMAD.WIDE.U32.X adds its carry-in predicate", "[video_core][ampere]") {
    // Captured: IMAD.WIDE.U32.X R10, R15, RZ, R10, P2 (0f0a7225 000000ff 010e040a). ptxas emits
    // the same form, with a register multiplier, for the final step of mul.hi.u64.
    constexpr std::array<std::array<u32, 4>, 6> samples{{
        {0, 0, 0, 0},
        {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
        {0x00000001, 0x00000000, 0xffffffff, 0xffffffff},
        {0x12345678, 0x9abcdef0, 0xfedcba98, 0x01234567},
        {0xffffffff, 0x00000001, 0xffffffff, 0x7fffffff},
        {0xdeadbeef, 0xcafef00d, 0x80000000, 0xfffffffe},
    }};
    for (const u32 carry_predicate : {2U, 10U, 7U}) { // P2, !P2, PT
        for (const bool register_b : {false, true}) {
            const u32 rd = 10, ra = 15, rb = register_b ? 20U : 255U;
            const Instruction native{{0x00007225U | (rd << 16) | (ra << 24), rb,
                                      0x000e040aU | (carry_predicate << 23), 0x003fde00}};
            const auto result = LowerToMaxwell(std::array{native, Exit});
            REQUIRE(static_cast<bool>(result));
            for (const auto& sample : samples) {
                for (const bool p2 : {false, true}) {
                    Machine machine;
                    for (u32 i = 0; i < 255; ++i) machine.regs[i] = 0x5a5a0000U + i;
                    machine.preds[2] = p2;
                    machine.regs[ra] = sample[0];
                    if (register_b) machine.regs[rb] = sample[1];
                    machine.regs[rd] = sample[2];
                    machine.regs[rd + 1] = sample[3];
                    for (const u64 word : result.instructions) {
                        if ((word >> 52) == 0xe30) break; // EXIT
                        machine.Run(word);
                    }
                    const bool carry = carry_predicate == 7 ? true : (carry_predicate == 2) == p2;
                    const u64 addend = u64{sample[2]} | (u64{sample[3]} << 32);
                    const u64 expected = u64{sample[0]} * (register_b ? sample[1] : 0U) + addend + carry;
                    CHECK(machine.regs[rd] == static_cast<u32>(expected));
                    CHECK(machine.regs[rd + 1] == static_cast<u32>(expected >> 32));
                    CHECK(machine.preds[2] == p2);
                }
            }
        }
    }
}

TEST_CASE("Ampere IMAD.X rejects carry-in forms without a proven lowering", "[video_core][ampere]") {
    const Instruction wide{{0x0f0a7225, 0x000000ff, 0x010e040a, 0x003fde00}};
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{wide, Exit})));
    Instruction carry_out = wide; // .X together with a carry-out predicate.
    carry_out.words[2] = (carry_out.words[2] & ~(7U << 17)) | (1U << 17);
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{carry_out, Exit})));
    Instruction high = wide; // IMAD.HI.U32.X
    high.words[0] = (high.words[0] & ~0xfffU) | 0x227;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{high, Exit})));
    Instruction signed_product = wide;
    signed_product.words[2] |= 1U << 9;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{signed_product, Exit})));
}

TEST_CASE("Ampere IMAD.X with a constant-buffer addend adds its carry-in predicate", "[video_core][ampere]") {
    // Captured: IMAD.X R29, R22, R29, c[0x4][0x118], PT (161d7624 01004600 038e061d); ptxas emits
    // IMAD.X R7, R0, R7, c[0x0][0x170], P0 for madc.lo.u32 after add.cc.u32.
    constexpr std::array<std::array<u32, 3>, 5> samples{{
        {0, 0, 0},
        {0xffffffff, 0xffffffff, 0xffffffff},
        {0x12345678, 0x9abcdef0, 0xfedcba98},
        {0x0000ffff, 0x00010001, 0x7fffffff},
        {0xdeadbeef, 0xcafef00d, 0x80000000},
    }};
    for (const u32 carry_predicate : {7U, 15U, 0U, 8U}) { // PT, !PT, P0, !P0
        const u32 rd = 29, ra = 22, rb = 29;
        const Instruction native{{0x00007624U | (rd << 16) | (ra << 24), 0x01004600,
                                  0x000e0600U | rb | (carry_predicate << 23), 0x000fe200}};
        const auto result = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(result));
        for (const auto& sample : samples) {
            for (const bool p0 : {false, true}) {
                Machine machine;
                for (u32 i = 0; i < 255; ++i) machine.regs[i] = 0x5a5a0000U + i;
                machine.preds[0] = p0;
                machine.regs[ra] = sample[0];
                machine.regs[rb] = sample[1];
                machine.cbufs[4][0x118 / 4] = sample[2];
                for (const u64 word : result.instructions) {
                    if ((word >> 52) == 0xe30) break; // EXIT
                    machine.Run(word);
                }
                const bool value = (carry_predicate & 7) == 7 ? true : p0;
                const bool carry = (carry_predicate & 8) ? !value : value;
                CHECK(machine.regs[rd] == sample[0] * sample[1] + sample[2] + (carry ? 1U : 0U));
                CHECK(machine.regs[ra] == sample[0]);
            }
        }
    }
    Instruction register_form = Instruction{{0x161d7224, 0x00000021, 0x078e061d, 0x000fe200}};
    register_form.words[2] |= 1U << 10; // .X on the register-addend form stays unproven.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{register_form, Exit})));
}

TEST_CASE("Ampere LEA with a negated constant subtracts it", "[video_core][ampere]") {
    // Captured: LEA R0, R12, -c[0x3][0x28], 0x3 (0c007a11 80c00a00 078e18ff); ptxas emits the same
    // form for (a << 3) - u32 parameter.
    constexpr std::array<std::array<u32, 2>, 5> samples{{
        {0, 0}, {1, 9}, {0xffffffff, 1}, {0x12345678, 0x9abcdef0}, {0x20000000, 0x80000000},
    }};
    for (const u32 shift : {0U, 3U, 31U}) {
        const Instruction native{{0x0c007a11, 0x80c00a00, 0x078e00ffU | (shift << 11), 0x001fc400}};
        const auto result = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(result));
        for (const auto& sample : samples) {
            Machine machine;
            machine.regs[12] = sample[0];
            machine.cbufs[3][0x28 / 4] = sample[1];
            for (const u64 word : result.instructions) {
                if ((word >> 52) == 0xe30) break; // EXIT
                machine.Run(word);
            }
            CHECK(machine.regs[0] == (sample[0] << shift) - sample[1]);
        }
    }
    Instruction carry_out{{0x0c007a11, 0x80c00a00, 0x078018ff, 0x001fc400}}; // P0 carry-out.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{carry_out, Exit})));
    Instruction absolute{{0x0c007a11, 0x40c00a00, 0x078e18ff, 0x001fc400}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute, Exit})));
}
