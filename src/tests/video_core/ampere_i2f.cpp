// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

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

/// Checks uniform I2F signedness and half selection, and rejects invalid uniform operands.
TEST_CASE("Ampere I2F converts either half of signed and unsigned uniform words", "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const u32 half : {0U, 1U}) {
            // @P0 I2F.{U,S}16 R0, UR4(.H1), used by TotK's menu.
            const Instruction convert{{0x00000d06, 4U | (half << 28),
                                       0x08101000U | (static_cast<u32>(is_signed) << 10), 0}};
            const auto lowered = LowerToMaxwell(std::array{convert, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::I2F_reg);
            REQUIRE((word & 255) == 0);
            REQUIRE(((word >> 16) & 15) == 0);
            REQUIRE(((word >> 8) & 3) == 2); // F32
            REQUIRE(((word >> 10) & 3) == 1); // 16-bit source.
            REQUIRE(((word >> 13) & 1) == static_cast<u64>(is_signed));
            REQUIRE(((word >> 20) & 255) == 220); // UR4's mirror.
            REQUIRE(((word >> 41) & 7) == half * 2);
        }
    }
    for (const u32 invalid : {0x20000004U, 64U}) {
        const Instruction convert{{0x00007d06, invalid, 0x08101000, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{convert, EXIT})));
    }
}

} // namespace
