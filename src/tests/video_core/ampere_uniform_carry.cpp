// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// The predicated register/predicate subset a guarded ULEA with a carry lowers to, following
// Eglantine's Maxwell translators (ISCADD, ISETP, P2R, R2P, MOV).
struct Machine {
    std::array<u32, 256> regs{};
    std::array<bool, 8> preds{false, false, false, false, false, false, false, true};
    u32 Reg(u64 word, u32 bit) const {
        const u32 index = static_cast<u32>((word >> bit) & 255);
        return index == 255 ? 0U : regs[index];
    }
    void Run(u64 word) {
        const u32 guard = static_cast<u32>((word >> 16) & 15);
        if (preds[guard & 7] == ((guard & 8) != 0)) return;
        const u32 dest = static_cast<u32>(word & 255);
        const auto write = [&](u32 value) { if (dest != 255) regs[dest] = value; };
        const u64 top = word >> 48;
        if (top == 0x5c18) { // ISCADD (reg)
            REQUIRE(((word >> 47) & 7) == 0);
            return write((Reg(word, 8) << ((word >> 39) & 31)) + Reg(word, 20));
        }
        if (top == 0x5c98) return write(Reg(word, 20)); // MOV (reg)
        if (top == 0x38e8) { // P2R (imm, PR mode)
            const u32 mask = static_cast<u32>((word >> 20) & 0x7f);
            REQUIRE(((word >> 40) & 7) == 0);
            u32 bits = 0;
            for (u32 i = 0; i < 7; ++i) bits |= preds[i] && ((mask >> i) & 1) ? 1U << i : 0U;
            return write((Reg(word, 8) & ~mask) | bits);
        }
        if (top == 0x38f0) { // R2P (imm, PR mode)
            const u32 mask = static_cast<u32>((word >> 20) & 0x7f), value = Reg(word, 8);
            REQUIRE(((word >> 40) & 7) == 0);
            for (u32 i = 0; i < 7; ++i) if ((mask >> i) & 1) preds[i] = (value >> i) & 1;
            return;
        }
        if (top == 0x5b60 || top == 0x5b61 || top == 0x5b62 || top == 0x5b63) { // ISETP (reg), unsigned
            REQUIRE((word & 7) == 7);
            REQUIRE(((word >> 39) & 15) == 7); // AND with PT.
            REQUIRE(((word >> 43) & 15) == 0);
            REQUIRE(((word >> 48) & 1) == 0);
            const u32 a = Reg(word, 8), b = Reg(word, 20), op = static_cast<u32>((word >> 49) & 7);
            REQUIRE(op == 1);
            const u32 target = static_cast<u32>((word >> 3) & 7);
            if (target != 7) preds[target] = a < b;
            return;
        }
        FAIL("unmodelled Maxwell instruction " << std::hex << word);
    }
};
} // Anonymous namespace

TEST_CASE("Ampere guarded ULEA with a uniform carry keeps the guard and guest predicates",
          "[video_core][ampere]") {
    // Captured Drag x Drive form @!UP6 ULEA UR30, UP0, UR40, UR30, 0x4 (281ee291 0000001e
    // 0f80203f), with UR4 = (UR5 << 4) + UR6 so the uniforms are in the low mirror (UR n = R216+n).
    constexpr u32 ur_d = 4, ur_a = 5, ur_b = 6;
    for (const u32 guard_up : {6U, 3U}) {
        for (const u32 carry_up : {0U, 5U}) {
            for (const bool negated_guard : {false, true}) {
                const u32 guard_field = (negated_guard ? 8U : 0U) | guard_up;
                const Instruction native{{0x00000291U | (guard_field << 12) | (ur_d << 16) | (ur_a << 24),
                                          ur_b, (0x0f80203fU & ~(7U << 17)) | (carry_up << 17), 0x000fe200}};
                const auto lowered = LowerToMaxwell(std::array{native, Exit});
                REQUIRE(static_cast<bool>(lowered));
                constexpr std::array<std::array<u32, 2>, 4> samples{{
                    {0x00000001, 0x00000002}, {0x0fffffff, 0x00000010}, {0x10000000, 0xffffffff},
                    {0xffffffff, 0xffffffff},
                }};
                for (const auto& sample : samples) {
                    for (u32 state = 0; state < 16; ++state) {
                        const bool guard_value = state & 1, carry_before = state & 2;
                        const bool guest_guard_slot = state & 4, guest_carry_slot = state & 8;
                        Machine machine;
                        for (u32 i = 0; i < 255; ++i) machine.regs[i] = 0x5a5a0000U + i;
                        machine.regs[216 + ur_a] = sample[0];
                        machine.regs[216 + ur_b] = sample[1];
                        const u32 old_d = machine.regs[216 + ur_d];
                        u32 r239 = 0x2aU; // Unrelated uniform predicates.
                        r239 = (r239 & ~(1U << (6 - guard_up))) | (guard_value ? 1U << (6 - guard_up) : 0U);
                        r239 = (r239 & ~(1U << (6 - carry_up))) | (carry_before ? 1U << (6 - carry_up) : 0U);
                        machine.regs[239] = r239;
                        machine.preds[6 - guard_up] = guest_guard_slot;
                        machine.preds[6 - carry_up] = guest_carry_slot;
                        const auto other_preds = machine.preds;
                        for (const u64 word : lowered.instructions) {
                            if ((word >> 52) == 0xe30) break; // EXIT
                            machine.Run(word);
                        }
                        const bool executes = guard_value != negated_guard;
                        const u32 sum = (sample[0] << 4) + sample[1];
                        const bool carry = executes ? sum < sample[1] : carry_before;
                        CHECK(machine.regs[216 + ur_d] == (executes ? sum : old_d));
                        const u32 expected_r239 = (r239 & ~(1U << (6 - carry_up))) | (carry ? 1U << (6 - carry_up) : 0U);
                        CHECK(machine.regs[239] == expected_r239);
                        CHECK(machine.preds == other_preds);
                    }
                }
            }
        }
    }
}

TEST_CASE("Ampere guarded ULEA rejects a carry into its own guard", "[video_core][ampere]") {
    const Instruction same{{0x281ee291, 0x0000001e, 0x0f8c203f, 0x000fe200}}; // @!UP6 ... UP6
    const auto lowered = LowerToMaxwell(std::array{same, Exit});
    REQUIRE_FALSE(static_cast<bool>(lowered));
    CHECK(lowered.error == "native uniform arithmetic predication");
}

namespace {
// Adds the unpredicated/predicated ALU subset right_shift_u64_low emits (MOV32I, IADD with a
// negated b, SHR/SHL by register, LOP OR, IADD32I) to the predicate machine above.
struct ShiftMachine : Machine {
    void Run(u64 word) {
        const u32 guard = static_cast<u32>((word >> 16) & 15);
        if (preds[guard & 7] == ((guard & 8) != 0)) return;
        const u32 dest = static_cast<u32>(word & 255);
        const auto write = [&](u32 value) { if (dest != 255) regs[dest] = value; };
        const u64 top = word >> 48;
        if ((word >> 56) == 0x01) return write(static_cast<u32>(word >> 20));
        if ((word >> 56) == 0x1c) return write(Reg(word, 8) + static_cast<u32>(word >> 20));
        if (top == 0x5c10 || top == 0x5c11) {
            REQUIRE(((word >> 43) & 0x7f) == 0x20); // Only bit 48 (negate b).
            return write(Reg(word, 8) - Reg(word, 20));
        }
        if (top == 0x5c28 || top == 0x5c48) { // SHR / SHL (reg), unsigned; clamping unless .W
            const bool wrap = (word >> 39) & 1;
            const u32 shift = wrap ? Reg(word, 20) & 31 : Reg(word, 20), a = Reg(word, 8);
            return write(shift >= 32 ? 0U : top == 0x5c28 ? a >> shift : a << shift);
        }
        if ((word >> 53) == ((0x04ULL << 3) | 2)) // LOP32I.XOR
            return write(Reg(word, 8) ^ static_cast<u32>(word >> 20));
        if (top == 0x5c47) { // LOP (reg) .OR, no inversions or predicate output
            REQUIRE(((word >> 39) & 0x3f) == 4); // op (bits 41-42) = OR
            return write(Reg(word, 8) | Reg(word, 20));
        }
        Machine::Run(word);
    }
};
} // Anonymous namespace

TEST_CASE("Ampere guarded USHF.R.U64 shifts the uniform pair", "[video_core][ampere]") {
    // Captured Drag x Drive: @UP3 USHF.R.U64 UR33, UR32, UR34, UR33 (20213299 00000022 08001221),
    // here with UR4 = (UR5:UR4 >> UR6).low so the uniforms are in the low mirror.
    const Instruction native{{0x04043299, 0x00000006, 0x08001205, 0x001fc600}};
    const auto lowered = LowerToMaxwell(std::array{native, Exit});
    REQUIRE(static_cast<bool>(lowered));
    constexpr std::array<std::array<u32, 3>, 7> samples{{
        {0x89abcdef, 0x01234567, 0}, {0x89abcdef, 0x01234567, 4}, {0x89abcdef, 0x01234567, 31},
        {0x89abcdef, 0x01234567, 32}, {0x89abcdef, 0x01234567, 40}, {0xffffffff, 0xffffffff, 63},
        {0x00000001, 0x80000000, 1},
    }};
    for (const auto& sample : samples) {
        for (const bool guard : {false, true}) {
            ShiftMachine machine;
            for (u32 i = 0; i < 255; ++i) machine.regs[i] = 0x5a5a0000U + i;
            machine.regs[220] = sample[0];
            machine.regs[221] = sample[1];
            machine.regs[222] = sample[2];
            machine.regs[239] = guard ? 1U << 3 : 0U; // UP3 in R239 bit 3.
            machine.preds[3] = !guard;                // Guest P3 differs from UP3.
            for (const u64 word : lowered.instructions) {
                if ((word >> 52) == 0xe30) break;
                machine.Run(word);
            }
            const u64 pair = u64{sample[0]} | (u64{sample[1]} << 32);
            CHECK(machine.regs[220] == (guard ? static_cast<u32>(pair >> sample[2]) : sample[0]));
            CHECK(machine.preds[3] == !guard);
        }
    }
}

TEST_CASE("Ampere BMSK matches the hardware's clamp and wrap masks", "[video_core][ampere]") {
    // {position, width, BMSK, BMSK.W} measured on an RTX 5080 from PTX bmsk.clamp.b32 and
    // bmsk.wrap.b32, which ptxas SM86 encodes as 0x721b with bit 75 for .W (the captured Star
    // Fox form is BMSK R5, R4, R5: 0405721b 00000005 00000000).
    constexpr std::array<std::array<u32, 4>, 81> measured{{
        {0x0, 0x0, 0x00000000, 0x00000000},
        {0x0, 0x1, 0x00000001, 0x00000001},
        {0x0, 0x3, 0x00000007, 0x00000007},
        {0x0, 0x10, 0x0000ffff, 0x0000ffff},
        {0x0, 0x1f, 0x7fffffff, 0x7fffffff},
        {0x0, 0x20, 0xffffffff, 0x00000000},
        {0x0, 0x21, 0xffffffff, 0x00000001},
        {0x0, 0x28, 0xffffffff, 0x000000ff},
        {0x0, 0xffffffff, 0xffffffff, 0x7fffffff},
        {0x1, 0x0, 0x00000000, 0x00000000},
        {0x1, 0x1, 0x00000002, 0x00000002},
        {0x1, 0x3, 0x0000000e, 0x0000000e},
        {0x1, 0x10, 0x0001fffe, 0x0001fffe},
        {0x1, 0x1f, 0xfffffffe, 0xfffffffe},
        {0x1, 0x20, 0xfffffffe, 0x00000000},
        {0x1, 0x21, 0xfffffffe, 0x00000002},
        {0x1, 0x28, 0xfffffffe, 0x000001fe},
        {0x1, 0xffffffff, 0xfffffffe, 0xfffffffe},
        {0x5, 0x0, 0x00000000, 0x00000000},
        {0x5, 0x1, 0x00000020, 0x00000020},
        {0x5, 0x3, 0x000000e0, 0x000000e0},
        {0x5, 0x10, 0x001fffe0, 0x001fffe0},
        {0x5, 0x1f, 0xffffffe0, 0xffffffe0},
        {0x5, 0x20, 0xffffffe0, 0x00000000},
        {0x5, 0x21, 0xffffffe0, 0x00000020},
        {0x5, 0x28, 0xffffffe0, 0x00001fe0},
        {0x5, 0xffffffff, 0xffffffe0, 0xffffffe0},
        {0x10, 0x0, 0x00000000, 0x00000000},
        {0x10, 0x1, 0x00010000, 0x00010000},
        {0x10, 0x3, 0x00070000, 0x00070000},
        {0x10, 0x10, 0xffff0000, 0xffff0000},
        {0x10, 0x1f, 0xffff0000, 0xffff0000},
        {0x10, 0x20, 0xffff0000, 0x00000000},
        {0x10, 0x21, 0xffff0000, 0x00010000},
        {0x10, 0x28, 0xffff0000, 0x00ff0000},
        {0x10, 0xffffffff, 0xffff0000, 0xffff0000},
        {0x1f, 0x0, 0x00000000, 0x00000000},
        {0x1f, 0x1, 0x80000000, 0x80000000},
        {0x1f, 0x3, 0x80000000, 0x80000000},
        {0x1f, 0x10, 0x80000000, 0x80000000},
        {0x1f, 0x1f, 0x80000000, 0x80000000},
        {0x1f, 0x20, 0x80000000, 0x00000000},
        {0x1f, 0x21, 0x80000000, 0x80000000},
        {0x1f, 0x28, 0x80000000, 0x80000000},
        {0x1f, 0xffffffff, 0x80000000, 0x80000000},
        {0x20, 0x0, 0x00000000, 0x00000000},
        {0x20, 0x1, 0x00000000, 0x00000001},
        {0x20, 0x3, 0x00000000, 0x00000007},
        {0x20, 0x10, 0x00000000, 0x0000ffff},
        {0x20, 0x1f, 0x00000000, 0x7fffffff},
        {0x20, 0x20, 0x00000000, 0x00000000},
        {0x20, 0x21, 0x00000000, 0x00000001},
        {0x20, 0x28, 0x00000000, 0x000000ff},
        {0x20, 0xffffffff, 0x00000000, 0x7fffffff},
        {0x21, 0x0, 0x00000000, 0x00000000},
        {0x21, 0x1, 0x00000000, 0x00000002},
        {0x21, 0x3, 0x00000000, 0x0000000e},
        {0x21, 0x10, 0x00000000, 0x0001fffe},
        {0x21, 0x1f, 0x00000000, 0xfffffffe},
        {0x21, 0x20, 0x00000000, 0x00000000},
        {0x21, 0x21, 0x00000000, 0x00000002},
        {0x21, 0x28, 0x00000000, 0x000001fe},
        {0x21, 0xffffffff, 0x00000000, 0xfffffffe},
        {0x28, 0x0, 0x00000000, 0x00000000},
        {0x28, 0x1, 0x00000000, 0x00000100},
        {0x28, 0x3, 0x00000000, 0x00000700},
        {0x28, 0x10, 0x00000000, 0x00ffff00},
        {0x28, 0x1f, 0x00000000, 0xffffff00},
        {0x28, 0x20, 0x00000000, 0x00000000},
        {0x28, 0x21, 0x00000000, 0x00000100},
        {0x28, 0x28, 0x00000000, 0x0000ff00},
        {0x28, 0xffffffff, 0x00000000, 0xffffff00},
        {0xffffffff, 0x0, 0x00000000, 0x00000000},
        {0xffffffff, 0x1, 0x00000000, 0x80000000},
        {0xffffffff, 0x3, 0x00000000, 0x80000000},
        {0xffffffff, 0x10, 0x00000000, 0x80000000},
        {0xffffffff, 0x1f, 0x00000000, 0x80000000},
        {0xffffffff, 0x20, 0x00000000, 0x00000000},
        {0xffffffff, 0x21, 0x00000000, 0x80000000},
        {0xffffffff, 0x28, 0x00000000, 0x80000000},
        {0xffffffff, 0xffffffff, 0x00000000, 0x80000000}
    }};
    for (const bool wrap : {false, true}) {
        const Instruction native{{0x0405721b, 0x00000005, wrap ? 0x800U : 0U, 0x000fe400}};
        const auto lowered = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(lowered));
        for (const auto& [position, width, clamped, wrapped] : measured) {
            ShiftMachine machine;
            machine.regs[4] = position;
            machine.regs[5] = width;
            for (const u64 word : lowered.instructions) {
                if ((word >> 52) == 0xe30) break;
                machine.Run(word);
            }
            CHECK(machine.regs[5] == (wrap ? wrapped : clamped));
            CHECK(machine.regs[4] == position);
        }
    }
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{
        Instruction{{0x0405721b, 0x00000005, 0x00001000, 0}}, Exit}))); // Unknown mode bit.
}

namespace {
struct LogicMachine : Machine {
    void Run(u64 word) {
        const u32 guard = static_cast<u32>((word >> 16) & 15);
        if (preds[guard & 7] == ((guard & 8) != 0)) return;
        const u64 top = word >> 52;
        if (top == 0x5be) { // LOP3 (reg): LUT(a, b, c), Pd = result != 0
            const u32 a = Reg(word, 8), b = Reg(word, 20), c = Reg(word, 39);
            const u32 lut = static_cast<u32>((word >> 28) & 255);
            u32 value = 0;
            for (u32 bit = 0; bit < 32; ++bit) {
                const u32 index = (((a >> bit) & 1) << 2) | (((b >> bit) & 1) << 1) | ((c >> bit) & 1);
                value |= ((lut >> index) & 1) << bit;
            }
            if ((word & 255) != 255) regs[word & 255] = value;
            const u32 target = static_cast<u32>((word >> 48) & 7);
            REQUIRE(((word >> 36) & 3) == (target == 7 ? 0U : 3U));
            if (target != 7) preds[target] = value != 0;
            return;
        }
        if (top == 0x509) { // PSETP
            const auto pred = [&](u32 index, bool negate) { return preds[index & 7] != negate; };
            const auto combine = [](bool x, bool y, u32 op) { return op == 0 ? x && y : op == 1 ? x || y : x != y; };
            const bool a = pred(static_cast<u32>((word >> 12) & 7), (word >> 15) & 1);
            const bool b = pred(static_cast<u32>((word >> 29) & 7), (word >> 32) & 1);
            const bool c = pred(static_cast<u32>((word >> 39) & 7), (word >> 42) & 1);
            const u32 bop1 = static_cast<u32>((word >> 24) & 3), bop2 = static_cast<u32>((word >> 45) & 3);
            const u32 dest_a = static_cast<u32>((word >> 3) & 7), dest_b = static_cast<u32>(word & 7);
            const bool result_a = combine(combine(a, b, bop1), c, bop2);
            const bool result_b = combine(combine(!a, b, bop1), c, bop2);
            if (dest_a != 7) preds[dest_a] = result_a;
            if (dest_b != 7) preds[dest_b] = result_b;
            return;
        }
        Machine::Run(word);
    }
};
} // Anonymous namespace

TEST_CASE("Ampere LOP3 combines its predicate result with a predicate input", "[video_core][ampere]") {
    // Captured Star Fox: LOP3.LUT P0, RZ, R1, c[0x0][0x14], RZ, 0xfc, P0 (01ff7a12 00000500
    // 0000fcff). ptxas emits bit 80 = 0 for lop3.or.b32 d|p and 1 (.PAND) for lop3.and.b32, and an
    // RTX 5080 gives p = (d != 0) {or, and} q for both.
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{
        Instruction{{0x01ff7a12, 0x00000500, 0x0000fcff, 0x000fda00}}, Exit})));
    for (const bool pand : {false, true}) {
        for (const u32 target : {0U, 3U}) {
            for (const u32 input : {0U, 8U, 3U, 7U}) { // P0, !P0, P3, PT
                for (const u32 guard : {7U, 5U}) {
                    const Instruction native{{0x01ff0212U | (guard << 12), 0x00000002,
                        0x0000fcffU | (target << 17) | (input << 23) | (pand ? 1U << 16 : 0U), 0}};
                    const auto lowered = LowerToMaxwell(std::array{native, Exit});
                    REQUIRE(static_cast<bool>(lowered));
                    for (u32 state = 0; state < 64; ++state) {
                        LogicMachine machine;
                        machine.regs[1] = state & 1 ? 0x100U : 0U;
                        machine.regs[2] = state & 2 ? 0x8U : 0U;
                        for (u32 p = 0; p < 4; ++p) machine.preds[p] = (state >> (2 + p)) & 1;
                        machine.preds[5] = false; // The @P5 guard, when used, is false.
                        auto expected = machine.preds;
                        if (guard == 7) {
                            const bool nonzero = (machine.regs[1] | machine.regs[2]) != 0;
                            const bool q = (input & 7) == 7 ? true : machine.preds[input & 7] != ((input & 8) != 0);
                            expected[target] = pand ? nonzero && q : nonzero || q;
                        }
                        for (const u64 word : lowered.instructions) {
                            if ((word >> 52) == 0xe30) break;
                            machine.Run(word);
                        }
                        CHECK(machine.preds == expected);
                    }
                }
            }
        }
    }
}
