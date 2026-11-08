// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

bool Contains(const Shader::Ampere::LoweredProgram& lowered, u64 instruction) {
    return std::find(lowered.instructions.begin(), lowered.instructions.end(), instruction) !=
           lowered.instructions.end();
}

TEST_CASE("Ampere I2F.F32.S16 lowers to a signed 16-bit Maxwell I2F", "[video_core][ampere]") {
    // I2F.F32.S16 R1, R1 and R3, R3 (Mario Tennis Fever's text pixel shaders).
    const std::array<Instruction, 3> code{{
        {{0x00017306, 0x00000001, 0x00101400, 0x020ea200}},
        {{0x00037306, 0x00000003, 0x00101400, 0x000e2400}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    // Maxwell I2F.F32.S16: source format 1 (bits 10-11), signed (bit 13).
    REQUIRE(Contains(lowered, 0x5cb8000000172601ULL));
    REQUIRE(Contains(lowered, 0x5cb8000000372603ULL));
}

TEST_CASE("Ampere I2F keeps unsigned 16-bit sources unsigned", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00017306, 0x00000001, 0x00101000, 0}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered, 0x5cb8000000170601ULL));
}

TEST_CASE("Ampere I2F converts 16-bit uniform sources", "[video_core][ampere]") {
    // Star Fox's graphics shader: I2F.F32.U16 R2, UR4.
    std::array<Instruction, 2> code{{
        {{0x00027d06, 0x00000004, 0x08101000, 0x000e6200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    // UR4 is mirrored in R220; preserve unsigned 16-bit conversion.
    REQUIRE(Contains(lowered, 0x5cb800000dc70602ULL));

    code[0] = {{0x00047d06, 0x10000004, 0x08101000, 0x000ef000}};
    const auto upper_half = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(upper_half));
    REQUIRE(Contains(upper_half, 0x5cb804000dc70604ULL));
    code[0].words[1] = 0x20000004;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));

    code[0] = {{0x00027d06, 0x00000004, 0x08101000, 0x000e6200}};

    code[0].words[2] |= 0x400;
    const auto signed_lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(signed_lowered));
    REQUIRE(Contains(signed_lowered, 0x5cb800000dc72602ULL));

    code[0].words[1] = 64;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

} // namespace
