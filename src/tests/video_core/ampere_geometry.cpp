// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::LoweringOptions;
constexpr Instruction exit_inst{{0x0000794d, 0, 0x03800000, 0}};

bool Contains(const Shader::Ampere::LoweredProgram& lowered, u64 instruction) {
    return std::find(lowered.instructions.begin(), lowered.instructions.end(), instruction) !=
           lowered.instructions.end();
}

constexpr std::array<Instruction, 5> geometry_program{{
    {{0x0004734e, 0, 0, 0}},          // LEPC R4
    {{0x00017923, 0, 0, 0}},          // ISBERD R1, [R0]
    {{0x00027324, 0xff, 0x4000, 0}},  // OUT.EMIT R2, R0, RZ
    {{0x02037924, 0, 0x8000, 0}},     // OUT.CUT R3, R2, 0
    exit_inst,
}};
} // namespace

TEST_CASE("Ampere geometry program counter uses the native address", "[video_core][ampere]") {
    const Instruction nop{{0x00007918, 0, 0, 0}};
    const Instruction lepc{{0x0014734e, 0, 0, 0}};
    const std::array code{nop, lepc, exit_inst};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    const auto result = LowerToMaxwell(code, LoweringOptions{
        .geometry = true, .native_code_address = 0x00000012fffffff0ULL});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 4);
    REQUIRE((result.instructions[1] & 0xff) == 20);
    REQUIRE(((result.instructions[1] >> 20) & 0xffffffffULL) == 0);
    REQUIRE((result.instructions[2] & 0xff) == 21);
    REQUIRE(((result.instructions[2] >> 20) & 0xffffffffULL) == 0x13);

    Instruction unsupported = lepc;
    unsupported.words[1] = 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst},
                                                   {.geometry = true})));
}

TEST_CASE("Ampere geometry stage map and exit setup are stage specific", "[video_core][ampere]") {
    const Instruction map_read{{0x05127923, 0, 0, 0}};
    const Instruction exit_setup{{0x03007357, 0, 0, 0}};
    const std::array code{map_read, exit_setup, exit_inst};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    const auto result = LowerToMaxwell(code, {.geometry = true});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 2);
    REQUIRE((result.instructions[0] & 0xff) == 18);
    REQUIRE(((result.instructions[0] >> 20) & 0xff) == 5);

    Instruction unsupported = exit_setup;
    unsupported.words[1] = 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst},
                                                   {.geometry = true})));
}

TEST_CASE("Ampere F2I supports 16-bit truncation", "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 9, 0x0021e800U | (is_signed ? 0x100U : 0), 0}};
        const auto result = LowerToMaxwell(std::array{convert, exit_inst});
        REQUIRE(static_cast<bool>(result));
        const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
            return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(word != result.instructions.end());
        REQUIRE(((*word >> 8) & 3) == 1);
        REQUIRE(((*word >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*word >> 39) & 3) == 3);
        REQUIRE(((*word >> 44) & 1) == 1);
    }
    const Instruction unsupported{{0x00067305, 9, 0x0021e801, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst})));
}

TEST_CASE("Ampere geometry programs lower their output instructions", "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(geometry_program, {.geometry = true});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered, 0x010000000007f004ULL)); // MOV32I R4, 0 (its own address)
    REQUIRE(Contains(lowered, 0x010000000007f005ULL)); // MOV32I R5, 0
    REQUIRE(Contains(lowered, 0x5c98078000070001ULL)); // MOV R1, R0
    REQUIRE(Contains(lowered, 0xfbe000800ff70002ULL)); // OUT.EMIT R2, R0, RZ
    REQUIRE(Contains(lowered, 0xf6e0010000070203ULL)); // OUT.CUT R3, R2, 0
}

TEST_CASE("Ampere output instructions stay rejected outside geometry programs",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(geometry_program);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "unsupported native opcode");
}
