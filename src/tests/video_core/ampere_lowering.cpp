// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere register SGXT clamps the full bit count before extraction", "[video_core][ampere]") {
    using Shader::Maxwell::Opcode;
    // SGXT.U32 R16, R21, R16: the destination also holds the bit count.
    for (const u32 predicate : {0U, 7U, 8U}) {
        const Instruction extend{{0x1510021aU | (predicate << 12), 16, 0, 0}};
        const auto lowered = LowerToMaxwell(std::array{extend, EXIT});
        REQUIRE(static_cast<bool>(lowered));
        for (const u32 count : {0U, 1U, 16U, 31U, 32U, 33U, 255U, 256U, 0xffffffffU}) {
            for (const u32 value : {0U, 0x80000001U, 0xffffffffU}) {
                std::array<u32, 256> registers{};
                registers[16] = count;
                registers[21] = value;
                for (const u64 word : lowered.instructions) {
                    const auto opcode = Shader::Maxwell::Decode(word);
                    if (opcode == Opcode::EXIT) break;
                    REQUIRE(((word >> 16) & 15) == predicate);
                    const u32 dest = word & 255;
                    const u32 source = registers[(word >> 8) & 255];
                    switch (opcode) {
                    case Opcode::IMNMX_imm:
                        REQUIRE(((word >> 39) & 15) == 7); // Unsigned minimum, PT.
                        REQUIRE(((word >> 48) & 1) == 0);
                        registers[dest] = std::min(source, static_cast<u32>((word >> 20) & 0x7ffff));
                        break;
                    case Opcode::SHL_imm:
                        registers[dest] = source << ((word >> 20) & 31);
                        break;
                    case Opcode::BFE_reg: {
                        const u32 control = registers[(word >> 20) & 255];
                        const u32 bits = (control >> 8) & 255;
                        REQUIRE((control & 255) == 0);
                        REQUIRE(bits <= 32);
                        REQUIRE(((word >> 48) & 1) == 0);
                        registers[dest] = bits == 32 ? source : source & ((1U << bits) - 1);
                        break;
                    }
                    default:
                        FAIL("Unexpected instruction in register SGXT");
                    }
                }
                const u32 expected = count >= 32 ? value : value & ((1U << count) - 1);
                REQUIRE(registers[16] == expected);
            }
        }
    }
    for (const u32 unsupported : {0x200U, 0x800U}) { // Signed and wrapping forms.
        const Instruction extend{{0x1510721a, 16, unsupported, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{extend, EXIT})));
    }
}

TEST_CASE("Ampere float-to-double conversion preserves its source and modifiers", "[video_core][ampere]") {
    for (const u32 source : {2U, 44U, 255U}) {
        for (const u32 modifiers : {0U, 0x40000000U, 0x80000000U, 0xc0000000U}) {
            const Instruction convert{{0x002c0310, source | modifiers, 0x00201800, 0}};
            const auto lowered = LowerToMaxwell(std::array{convert, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::F2F_reg);
            REQUIRE((word & 255) == 44);
            REQUIRE(((word >> 16) & 15) == 0); // @P0
            REQUIRE(((word >> 8) & 3) == 3); // F64 destination pair.
            REQUIRE(((word >> 10) & 3) == 2); // F32 source.
            REQUIRE(((word >> 20) & 255) == source);
            REQUIRE(((word >> 49) & 1) == ((modifiers >> 30) & 1));
            REQUIRE(((word >> 45) & 1) == (modifiers >> 31));
        }
    }
    for (const u32 dest : {43U, 254U, 255U}) {
        const Instruction convert{{0x00007310U | (dest << 16), 2, 0x00201800, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{convert, EXIT})));
    }
    const Instruction unsupported{{0x002c7310, 2, 0x00201801, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, EXIT})));
}

TEST_CASE("Ampere double-to-float conversion keeps rounding and aligned source pairs", "[video_core][ampere]") {
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        const Instruction convert{{0x00337310, 60, 0x00301000U | (rounding << 14), 0}};
        const auto lowered = LowerToMaxwell(std::array{convert, EXIT});
        REQUIRE(static_cast<bool>(lowered));
        const u64 word = lowered.instructions.front();
        REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::F2F_reg);
        REQUIRE((word & 255) == 51); // Single-register destinations may be odd.
        REQUIRE(((word >> 20) & 255) == 60);
        REQUIRE(((word >> 8) & 3) == 2);
        REQUIRE(((word >> 10) & 3) == 3);
        REQUIRE(((word >> 39) & 3) == rounding);
    }
    for (const u32 source : {1U, 254U, 255U}) {
        const Instruction convert{{0x00337310, source, 0x00301000, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{convert, EXIT})));
    }
}

TEST_CASE("Ampere double arithmetic retains constant-buffer and register operand order", "[video_core][ampere]") {
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        const std::array<Instruction, 4> operations{{
            {{0x2a267a2b, 0x00400200, 44U | (rounding << 14), 0}}, // DFMA R38, R42, c[1][8], R44
            {{0x1014762b, 0x00400200, 20U | (rounding << 14), 0}}, // DFMA R20, R16, R20, c[1][8]
            {{0x2c2e7229, 0, 46U | (rounding << 14), 0}}, // DADD R46, R44, R46
            {{0x26347629, 0x00400600, rounding << 14, 0}}, // DADD R52, R38, c[1][24]
        }};
        for (size_t index = 0; index < operations.size(); ++index) {
            const auto lowered = LowerToMaxwell(std::array{operations[index], EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            const std::array opcodes{Shader::Maxwell::Opcode::DFMA_cr, Shader::Maxwell::Opcode::DFMA_rc,
                                     Shader::Maxwell::Opcode::DADD_reg, Shader::Maxwell::Opcode::DADD_cbuf};
            REQUIRE(Shader::Maxwell::Decode(word) == opcodes[index]);
            REQUIRE((word & 255) == operations[index].Bits(16, 8));
            REQUIRE(((word >> 8) & 255) == operations[index].Bits(24, 8));
            REQUIRE(((word >> (index < 2 ? 50 : 39)) & 3) == rounding);
            if (index == 2) {
                REQUIRE(((word >> 20) & 255) == 46);
            } else {
                REQUIRE(((word >> 34) & 31) == 1);
                REQUIRE(((word >> 20) & 0x3fff) == (index == 3 ? 6U : 2U));
                if (index < 2) REQUIRE(((word >> 39) & 255) == operations[index].Bits(64, 8));
            }
        }
    }
}

TEST_CASE("Ampere double multiply retains the full constant-buffer operand", "[video_core][ampere]") {
    // DMUL R44, R44, c[1][0], reached after TotK's F2F.F64.F32.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U}) {
            const Instruction multiply{{0x2c2c7a28, 0x00400000U | ((negate & 1) << 31),
                                        (rounding << 14) | ((negate >> 1) << 8), 0}};
            const auto lowered = LowerToMaxwell(std::array{multiply, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DMUL_cbuf);
            REQUIRE((word & 255) == 44);
            REQUIRE(((word >> 8) & 255) == 44);
            REQUIRE(((word >> 34) & 31) == 1);
            REQUIRE(((word >> 20) & 0x3fff) == 0);
            REQUIRE(((word >> 39) & 3) == rounding);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
        }
    }
    const Instruction unaligned{{0x2c2c7a28, 0x00400100, 0, 0}}; // c[1][4]
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unaligned, EXIT})));
    const Instruction absolute{{0x2c2c7a28, 0x40400000, 0, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute, EXIT})));
}

TEST_CASE("Ampere double multiply lowers register operands", "[video_core][ampere]") {
    // DMUL R12, R36, R0, from a TotK graphics program.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U}) {
            const Instruction multiply{{0x240c7228, (negate & 1) << 31,
                                        (rounding << 14) | ((negate >> 1) << 8), 0}};
            const auto lowered = LowerToMaxwell(std::array{multiply, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DMUL_reg);
            REQUIRE((word & 255) == 12);
            REQUIRE(((word >> 8) & 255) == 36);
            REQUIRE(((word >> 20) & 255) == 0);
            REQUIRE(((word >> 39) & 3) == rounding);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
        }
    }
    for (const u32 second : {1U, 254U, 255U}) { // Odd, reserved or RZ second pairs.
        const Instruction multiply{{0x240c7228, second, 0, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{multiply, EXIT})));
    }
    const Instruction absolute_b{{0x240c7228, 0x40000000, 0, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute_b, EXIT})));
    const Instruction absolute_a{{0x240c7228, 0, 0x200, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute_a, EXIT})));
}

// BRA (unconditional) from `index` to `target`, both instruction indices.
Instruction Branch(s64 index, s64 target) {
    const u64 relative = static_cast<u64>((target - index - 1) * 16);
    return Instruction{{0x00007947, static_cast<u32>(relative),
                        0x03800000U | static_cast<u32>((relative >> 32) & 0x3ffff), 0}};
}

TEST_CASE("Ampere capture ends at the closing self-branch of a program without an unconditional EXIT", "[video_core][ampere]") {
    // TotK's cutscene video network: a loop left only through @P0 EXIT, then BRA to itself.
    constexpr Instruction PREDICATED_EXIT{{0x0000094d, 0, 0x03800000, 0}};
    const std::array<Instruction, 3> looping{{PREDICATED_EXIT, Branch(1, 0), Branch(2, 2)}};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(looping[0], 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(looping[1], 1) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(looping[2], 2) == ProgramCaptureState::Result::Complete);
    REQUIRE(static_cast<bool>(LowerToMaxwell(looping)));

    // A self-branch that an earlier branch jumps past is not the end.
    ProgramCaptureState skipped;
    REQUIRE(skipped.Observe(Branch(0, 3), 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(skipped.Observe(Branch(1, 1), 1) == ProgramCaptureState::Result::Continue);
}

TEST_CASE("Ampere double fused multiply-add lowers register operands", "[video_core][ampere]") {
    // DFMA R26, R34, R26, R12, from TotK's cutscene video network.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) { // -b, -a, -c
            const Instruction fma{{0x221a722b, 0x1aU | ((negate & 1) << 31),
                                   12U | (rounding << 14) | (((negate >> 1) & 1) << 8) |
                                       ((negate >> 2) << 11),
                                   0}};
            const auto lowered = LowerToMaxwell(std::array{fma, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DFMA_reg);
            REQUIRE((word & 255) == 26);
            REQUIRE(((word >> 8) & 255) == 34);
            REQUIRE(((word >> 20) & 255) == 26);
            REQUIRE(((word >> 39) & 255) == 12);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
            REQUIRE(((word >> 49) & 1) == (negate >> 2));
            REQUIRE(((word >> 50) & 3) == rounding);
        }
    }
    // |b|, |a| and |c| have no Maxwell DFMA form; odd or reserved pairs are rejected.
    for (const Instruction rejected : {Instruction{{0x221a722b, 0x4000001a, 12, 0}},
                                       Instruction{{0x221a722b, 0x1a, 12 | 0x200, 0}},
                                       Instruction{{0x221a722b, 0x1a, 12 | 0x400, 0}},
                                       Instruction{{0x221a722b, 0x1b, 12, 0}},
                                       Instruction{{0x221a722b, 0x1a, 13, 0}},
                                       Instruction{{0x221a722b, 0x1a, 254, 0}}}) {
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{rejected, EXIT})));
    }
}

TEST_CASE("Ampere UIMAD with an immediate addend lowers to an integer multiply-add", "[video_core][ampere]") {
    // UIMAD UR4, UR4, UR8, 0x3c00, from TotK's cutscene video network.
    for (const u32 sign : {0U, 0x200U}) {
        const Instruction multiply{{0x040474a4, 0x3c00, 0x0f8e0008U | sign, 0}};
        const auto lowered = LowerToMaxwell(std::array{multiply, EXIT});
        REQUIRE(static_cast<bool>(lowered));
        using Shader::Maxwell::Opcode;
        size_t xmads = 0;
        bool addend = false;
        for (const u64 word : lowered.instructions) {
            const Opcode opcode = Shader::Maxwell::Decode(word);
            xmads += opcode == Opcode::XMAD_reg || opcode == Opcode::XMAD_rc ||
                     opcode == Opcode::XMAD_cr || opcode == Opcode::XMAD_imm;
            addend |= opcode == Opcode::MOV32I && ((word >> 20) & 0xffffffffU) == 0x3c00;
        }
        REQUIRE(xmads == 3);
        REQUIRE(addend);
    }
}

// Lowered Maxwell words whose top bits (above `shift`) are `opcode`.
template <typename Program>
size_t CountMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    return static_cast<size_t>(std::count_if(lowered.instructions.begin(),
                                             lowered.instructions.end(),
                                             [&](u64 word) { return (word >> shift) == opcode; }));
}

template <typename Program>
u64 FindMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    const auto it = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                 [&](u64 word) { return (word >> shift) == opcode; });
    return it != lowered.instructions.end() ? *it : 0;
}

TEST_CASE("Ampere BRA.CONV always branches and BRA.DIV never does", "[video_core][ampere]") {
    // BRA.CONV UR6 / BRA.DIV ~URZ to the second EXIT: the mode is in the offset's low two bits.
    const std::array<Instruction, 3> convergent{{
        {{0x06007947, 0x10 | 3, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(convergent[0], 0) == ProgramCaptureState::Result::Continue);
    const auto taken = LowerToMaxwell(convergent);
    REQUIRE(static_cast<bool>(taken));
    REQUIRE(CountMaxwell(taken, 0xe24, 52) == 1);

    const std::array<Instruction, 3> divergent{{
        {{0xff007947, 0x10 | 2, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    const auto dropped = LowerToMaxwell(divergent);
    REQUIRE(static_cast<bool>(dropped));
    REQUIRE(CountMaxwell(dropped, 0xe24, 52) == 0);
}

TEST_CASE("Ampere TLD.SCR.LZ takes its handle from a uniform register", "[video_core][ampere]") {
    // TLD.SCR.LZ R5, R8, R4, R5, UR4, 0x0, 2D, 0x7 -> Maxwell TLD.B.
    const std::array<Instruction, 2> code{{
        {{0x04087f66, 0x30000405, 0x089e0705, 0x001f4200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 tld = FindMaxwell(lowered, 0xdd, 56);
    REQUIRE(tld != 0);
    REQUIRE(((tld >> 28) & 7) == 2); // 2D
    REQUIRE(((tld >> 31) & 0xf) == 7);
}

TEST_CASE("Ampere I2F converts signed 16-bit sources", "[video_core][ampere]") {
    // I2F.S16 R8, R8
    const std::array<Instruction, 2> code{{
        {{0x00087306, 0x00000008, 0x00101400, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 i2f = FindMaxwell(lowered, 0x5cb, 52);
    REQUIRE(((i2f >> 10) & 3) == 1); // 16-bit source
    REQUIRE(((i2f >> 13) & 1) == 1); // signed
}

TEST_CASE("Ampere integer-to-float conversions take immediates", "[video_core][ampere]") {
    // I2F.U32.RP R7, 0x20; I2FP.F32.U32.RZ R4, 0x100; WARPSYNC R7
    const std::array<Instruction, 4> code{{
        {{0x00077906, 0x00000020, 0x00209000, 0x000e2200}},
        {{0x00047845, 0x00000100, 0x0020d000, 0x000fe200}},
        {{0x00007348, 0x00000007, 0x03800000, 0x000fe800}},
        EXIT,
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere SUST.P keeps its channel mask", "[video_core][ampere]") {
    // SUST.P.2D.STRONG.SM.R.IGN [R6], R4, 0x0, 0x59
    const std::array<Instruction, 2> code{{
        {{0x06007b9b, 0x60005904, 0x0010a100, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xeb2, 52) >> 20) & 0xf) == 1);
}

TEST_CASE("Ampere SHFL with a register lane keeps its mode", "[video_core][ampere]") {
    // SHFL.DOWN PT, R7, R0, R5, 0x1f
    const std::array<Instruction, 2> code{{
        {{0x00077589, 0x08001f05, 0x000e0000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xef1, 52) >> 30) & 3) == 2);
}

TEST_CASE("Ampere calls find a return address set ahead of them", "[video_core][ampere]") {
    // MOV R9, 0x30; MOV R4, 0x5; CALL.REL.NOINC; EXIT; RET.REL.NODEC R9
    std::array<Instruction, 5> code{{
        {{0x00097802, 0x30, 0x00000f00, 0}},
        {{0x00047802, 0x5, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x09007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    // The link register is written again between its MOV and the call.
    code[1] = {{0x00097802, 0x5, 0x00000f00, 0}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere programs go on past an EXIT that a branch jumps over", "[video_core][ampere]") {
    // A call, then a branch over an EXIT to more of the program, with another call there.
    const std::array<Instruction, 8> code{{
        {{0x00047802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x50, 0x03c00000, 0}},
        {{0x00007947, 0x10, 0x03800000, 0}},
        EXIT,
        {{0x00047802, 0x60, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x04007950, 0xffffff80, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
}
} // namespace
