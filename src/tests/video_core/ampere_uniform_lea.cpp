// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere uniform LEA carry preserves aliased inputs and ordinary predicates",
          "[video_core][ampere]") {
    for (const u32 destination : {4U, 6U, 8U}) {
        for (const u32 carry_out : {0U, 1U, 6U}) {
            for (const u32 shift : {0U, 2U, 31U}) {
                const std::array<Instruction, 2> code{{
                    {{0x04007291U | (destination << 16), 6,
                      0x0f80003fU | (shift << 11) | (carry_out << 17), 0x001fc800}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                REQUIRE(lowered.instructions.size() == 7);
                const u32 slot = 6 - carry_out;
                const u64 mask = 1ULL << slot;
                REQUIRE(lowered.instructions[0] ==
                        (0x5c18000000070000ULL | 250ULL | (220ULL << 8) |
                         (222ULL << 20) | (u64{shift} << 39)));
                REQUIRE(lowered.instructions[1] ==
                        (0x38e8000000070000ULL | 253ULL | (255ULL << 8) | (mask << 20)));
                REQUIRE(lowered.instructions[2] ==
                        (0x5b60000000070000ULL | 7ULL | (u64{slot} << 3) |
                         (250ULL << 8) | (222ULL << 20) | (7ULL << 39) | (1ULL << 49)));
                REQUIRE(lowered.instructions[3] ==
                        (0x38e8000000070000ULL | 239ULL | (239ULL << 8) | (mask << 20)));
                REQUIRE(lowered.instructions[4] ==
                        (0x38f0000000070000ULL | (253ULL << 8) | (mask << 20)));
                REQUIRE(lowered.instructions[5] ==
                        (0x5c98000000070000ULL | (216ULL + destination) |
                         (15ULL << 39) | (250ULL << 20)));
            }
        }
    }
}

TEST_CASE("Ampere uniform LEA carry feeds uniform extended addition",
          "[video_core][ampere]") {
    const std::array<Instruction, 3> code{{
        {{0x04047291, 6, 0x0f80103f, 0x001fc800}},
        {{0x05057290, 7, 0x087fe43f, 0x000fc800}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 load_up0 = 0x38f0000000070000ULL | (239ULL << 8) | (64ULL << 20);
    REQUIRE(std::count(lowered.instructions.begin(), lowered.instructions.end(), load_up0) == 1);
    REQUIRE(lowered.source_instruction_offsets[1] == 6);
}

TEST_CASE("Ampere uniform LEA carry rejects unverified modes", "[video_core][ampere]") {
    for (const u32 modifier : {1U << 8, 1U << 9, 1U << 10, 1U << 16}) {
        const std::array<Instruction, 2> code{{
            {{0x04047291, 6, 0x0f80103fU | modifier, 0x001fc800}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    const std::array<Instruction, 2> predicated{{
        {{0x04040291, 6, 0x0f80103f, 0x001fc800}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(predicated)));
}

TEST_CASE("Ampere effective addresses accept a uniform addend", "[video_core][ampere]") {
    for (const u32 source : {4U, 26U, 27U, 63U}) {
        for (const u32 shift : {0U, 6U, 31U}) {
            const std::array<Instruction, 2> code{{
                {{0x04062c11, source, 0x0f8e00ffU | (shift << 11), 0x020fc800}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(lowered));
            const u32 reg = source < 22 ? 216 + source : source < 28 ? 218 + source : 255;
            REQUIRE(lowered.instructions.front() ==
                    (0x5c18000000020000ULL | 6ULL | (4ULL << 8) |
                     (u64{reg} << 20) | (u64{shift} << 39)));
        }
    }
    for (const u32 source : {28U, 255U, 0x104U}) {
        const std::array<Instruction, 2> code{{
            {{0x04067c11, source, 0x0f8e30ff, 0x020fc800}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    const std::array<Instruction, 2> carry{{
        {{0x04067c11, 4, 0x0f8030ff, 0x020fc800}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(carry)));
    auto scratch = carry;
    scratch[0].words[0] = 0xd8067c11;
    scratch[0].words[2] = 0x0f8e30ff;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(scratch)));
}

} // namespace
