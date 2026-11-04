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

} // namespace
