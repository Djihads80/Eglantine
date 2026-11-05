// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

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
TEST_CASE("Ampere half texture samples keep low and high channels in order",
          "[video_core][ampere][welcome-tour]") {
    for (const u32 mask : {3U, 7U, 15U}) {
        Instruction sample{{0x01017b60, 0x33c04402, 0x049e8702, 0x101fc200}};
        sample.words[2] = (sample.words[2] & ~0xfffU) | (mask << 8) |
                          (mask == 3 ? 255U : 2U);
        const std::array code{sample, EXIT};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        size_t packs{};
        for (const u64 word : lowered.instructions) {
            if ((word >> 48) != 0x5c18) continue;
            ++packs;
            REQUIRE(((word >> 8) & 0xff) == 247);
            REQUIRE(((word >> 20) & 0xff) == 246);
            REQUIRE(((word >> 39) & 0x1f) == 16);
        }
        REQUIRE(packs == (mask == 3 ? 1U : 2U));
    }
}

TEST_CASE("Ampere array depth samples retain their own descriptor bank",
          "[video_core][ampere][welcome-tour]") {
    const std::array<Instruction, 3> code{{
        {{0x01017b60, 0x33c04402, 0x049e8702, 0x101fc200}},
        {{0x0a0d7b60, 0xb1012400, 0x041e41ff, 0x00114400}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 15);
    const u64 load = FindMaxwell(lowered, 0x4c98, 48);
    REQUIRE(load != 0);
    REQUIRE(((load >> 34) & 0x1f) == 4);
    const u64 sample = FindMaxwell(lowered, 0xde, 56);
    REQUIRE(sample != 0);
    REQUIRE(((sample >> 20) & 0xff) == 246);
    REQUIRE(((sample >> 28) & 7) == 3);
    REQUIRE(((sample >> 50) & 1) == 1);
}

TEST_CASE("Ampere per-pixel gathers preserve both offset registers before aliased outputs",
          "[video_core][ampere][welcome-tour]") {
    const std::array<Instruction, 2> code{{
        {{0x040a7b63, 0x33800408, 0x041e2f08, 0x100fc200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 gather = FindMaxwell(lowered, 0xc8, 56);
    REQUIRE(gather != 0);
    REQUIRE((gather & 0xff) == 250);
    REQUIRE(((gather >> 20) & 0xff) == 246);
    REQUIRE(((gather >> 54) & 3) == 2);
    const auto copy = 0x5c98000000000000ULL | (u64{7} << 16) | 247 |
                      (u64{15} << 39) | (u64{9} << 20);
    const auto copied_offsets = std::find(lowered.instructions.begin(), lowered.instructions.end(), copy);
    const auto sample = std::find(lowered.instructions.begin(), lowered.instructions.end(), gather);
    REQUIRE(copied_offsets < sample);
}
} // namespace
