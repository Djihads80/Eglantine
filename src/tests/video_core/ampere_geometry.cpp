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

constexpr std::array<Instruction, 5> GEOMETRY_PROGRAM{{
    {{0x0004734e, 0, 0, 0}},          // LEPC R4
    {{0x00017923, 0, 0, 0}},          // ISBERD R1, [R0]
    {{0x00027324, 0xff, 0x4000, 0}},  // OUT.EMIT R2, R0, RZ
    {{0x02037924, 0, 0x8000, 0}},     // OUT.CUT R3, R2, 0
    EXIT,
}};

TEST_CASE("Ampere geometry programs lower their output instructions", "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(GEOMETRY_PROGRAM, true);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered, 0x010000000007f004ULL)); // MOV32I R4, 0 (its own address)
    REQUIRE(Contains(lowered, 0x010000000007f005ULL)); // MOV32I R5, 0
    REQUIRE(Contains(lowered, 0x5c98078000070001ULL)); // MOV R1, R0
    REQUIRE(Contains(lowered, 0xfbe000800ff70002ULL)); // OUT.EMIT R2, R0, RZ
    REQUIRE(Contains(lowered, 0xf6e0010000070203ULL)); // OUT.CUT R3, R2, 0
}

TEST_CASE("Ampere output instructions stay rejected outside geometry programs",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(GEOMETRY_PROGRAM);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "unsupported native opcode");
}

} // namespace
