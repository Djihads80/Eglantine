// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>
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

TEST_CASE("Ampere TLD4 lowers per-pixel offsets", "[video_core][ampere]") {
    // Welcome Tour's TLD4.PTP instruction: x/y in R4/R5 and packed per-tap offsets in R8/R9.
    const std::array<Instruction, 2> code{{
        {{0x040a7b63, 0x33800408, 0x041e2f08, 0x100fc200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));

    const u64 gather = FindMaxwell(lowered, 0xc8, 56);
    REQUIRE(gather != 0);
    CHECK(((gather >> 28) & 7) == 2); // 2D
    CHECK(((gather >> 20) & 0xff) == 246); // packed PTP offsets in the meta register
    CHECK(((gather >> 54) & 3) == 2); // per-pixel offsets
}

TEST_CASE("Ampere TLD4 rejects shadowed per-tap offset registers", "[video_core][ampere]") {
    std::array<Instruction, 2> code{{
        {{0x040a7b63, 0x338004f2, 0x041e2f08, 0x100fc200}},
        EXIT,
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
    code[0].words[1] = 0x338004f3;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    code[0].words[1] = 0x338004f4;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere TLD4 lowers a single offset register", "[video_core][ampere]") {
    std::array<Instruction, 2> code{{
        {{0x040a7b63, 0x33800408, 0x041e1f08, 0x100fc200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 gather = FindMaxwell(lowered, 0xc8, 56);
    REQUIRE(gather != 0);
    CHECK(((gather >> 54) & 3) == 1);
    code[0].words[2] |= 1U << 13;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere TLD4 packs half results in component order", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x040a7b63, 0x33800408, 0x041eaf08, 0x100fc200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    std::vector<u64> conversions;
    for (const u64 word : lowered.instructions) {
        if ((word >> 48) == 0x5ca8) {
            conversions.push_back(word);
        }
    }
    REQUIRE(conversions.size() == 4);
    CHECK(((conversions[0] >> 20) & 0xff) == 249);
    CHECK(((conversions[1] >> 20) & 0xff) == 248);
    CHECK(((conversions[2] >> 20) & 0xff) == 247);
    CHECK(((conversions[3] >> 20) & 0xff) == 246);
}

TEST_CASE("Ampere array depth samples use a second descriptor buffer", "[video_core][ampere]") {
    const std::array<Instruction, 3> code{{
        {{0x040a7b60, 0xb0000008, 0x000e41ff, 0}},
        {{0x040a7b60, 0xb0400008, 0x000e41ff, 0}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.required_texture_buffer == 0);
    CHECK(FindMaxwell(lowered, 0xc0, 56) != 0);
    CHECK(FindMaxwell(lowered, 0xde, 56) != 0);
}
} // namespace
