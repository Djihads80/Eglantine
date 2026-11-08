// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

TEST_CASE("Ampere surface reductions preserve 3D coordinates", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    // Star Fox: atomic unsigned add of R3 to the 3D image at c[0][0x88], xyz in R0-R2.
    Instruction sured{{0x0000759f, 0xa0002203, 0x0010e000, 0x0011e600}};
    for (const u32 dimension : {3U, 5U}) {
        sured.words[1] = (sured.words[1] & 0x1fffffffU) | (dimension << 29);
        const auto lowered = LowerToMaxwell(std::array{sured, exit});
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.required_texture_buffer == 0);
        REQUIRE(lowered.instructions.size() == 2);
        const u64 atomic = lowered.instructions[0];
        REQUIRE((atomic & 0xfff0000000000000ULL) == 0xeb40000000000000ULL);
        REQUIRE((atomic & 0xff) == 3); // data register
        REQUIRE(((atomic >> 8) & 0xff) == 0); // first coordinate
        REQUIRE(((atomic >> 16) & 15) == 7); // original guard
        REQUIRE(((atomic >> 20) & 7) == 0); // U32
        REQUIRE(((atomic >> 24) & 7) == 0); // ADD
        REQUIRE(((atomic >> 33) & 7) == dimension);
        REQUIRE(((atomic >> 36) & 0x1fff) == 0x22); // descriptor word offset
        REQUIRE(((atomic >> 51) & 1) == 1); // bound descriptor
    }
    sured.words[0] = 0xfd00759f; // R253-R255 would cross into the zero register in 3D
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{sured, exit})));
    sured.words[0] = 0x0a00859f; // xyz in R10-R12, guarded by !P0
    auto lowered = LowerToMaxwell(std::array{sured, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[0] >> 8) & 255) == 10);
    REQUIRE(((lowered.instructions[0] >> 16) & 15) == 8);
    sured = {{0x0000759f, 0xa0002403, 0x0310e000, 0x000fe200}};
    lowered = LowerToMaxwell(std::array{sured, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[0] >> 24) & 7) == 6); // OR
    REQUIRE(((lowered.instructions[0] >> 33) & 7) == 5); // still 3D
    sured.words[2] = 0x0010e000;
    sured.words[2] |= 1U << 23; // unimplemented atomic operation must still be rejected
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{sured, exit})));
}

TEST_CASE("Ampere surface atomic add returns the old 3D texel value", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    Instruction atomic{{0x00087593, 0xa0002407, 0x001ee000, 0x000ea400}};
    const auto lowered = LowerToMaxwell(std::array{atomic, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 0);
    REQUIRE(lowered.instructions.size() == 2);
    const u64 word = lowered.instructions[0];
    REQUIRE((word & 0xff80000000000000ULL) == 0xea00000000000000ULL);
    REQUIRE((word & 255) == 8); // old texel value returned to R8
    REQUIRE(((word >> 8) & 255) == 0); // xyz R0-R2
    REQUIRE(((word >> 20) & 255) == 7); // addend R7
    REQUIRE(((word >> 29) & 15) == 0); // ADD
    REQUIRE(((word >> 33) & 7) == 5); // 3D
    REQUIRE(((word >> 36) & 0x1fff) == 0x24); // c[0][0x90]
    REQUIRE(((word >> 51) & 7) == 0); // U32
    REQUIRE(((word >> 54) & 1) == 0); // bound descriptor
    atomic.words[2] &= ~(7U << 17); // writing a fault predicate is not yet supported
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{atomic, exit})));
}

TEST_CASE("Ampere gradient textures accept uniform array handles", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    // Star Fox's material shader: uniform handle UR4, coordinates R20, derivatives R16.
    Instruction txd{{0x14127f6c, 0xa0000410, 0x081e0f10, 0x001f4200}};
    auto lowered = LowerToMaxwell(std::array{txd, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    REQUIRE(lowered.instructions.size() == 10);
    REQUIRE(lowered.instructions[0] == 0x5c9807800dc700f8ULL);
    const u64 texture = lowered.instructions[4];
    REQUIRE((texture & 0xffc0000000000000ULL) == 0xde40000000000000ULL);
    REQUIRE(((texture >> 8) & 0xff) == 248);
    REQUIRE(((texture >> 20) & 0xff) == 16);
    REQUIRE(((texture >> 28) & 7) == 3);
    REQUIRE(((texture >> 31) & 15) == 15);
    REQUIRE((lowered.instructions[5] & 0xff) == 18);
    REQUIRE((lowered.instructions[7] & 0xff) == 16);

    txd = {{0x18320f6c, 0xa000041c, 0x0c1e0f34, 0x00414200}};
    lowered = LowerToMaxwell(std::array{txd, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[4] >> 49) & 1) == 1);
    REQUIRE(((lowered.instructions[4] >> 16) & 15) == 0);

    txd.words[1] = 0xa0004020; // invalid uniform handle
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{txd, exit})));
}

TEST_CASE("Ampere array gathers preserve the layer and split coordinates", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    const std::array<Instruction, 2> code{{
        {{0x201c7b63, 0xb0000a2b, 0x041e0f16, 0x000f4200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[0] >> 20) & 0xff) == 32);
    REQUIRE(((lowered.instructions[1] >> 20) & 0xff) == 33);
    REQUIRE(((lowered.instructions[2] >> 20) & 0xff) == 43);
    REQUIRE(((lowered.instructions[3] >> 28) & 7) == 3);
}

TEST_CASE("Ampere gathers keep coordinate pairs below reserved registers", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    struct Mode {
        Instruction instruction;
        u32 boundary;
        u32 high_uniforms;
        bool pair;
    };
    const std::array modes{
        Mode{{{0x201c7b63, 0xb0000a2b, 0x041e0f16, 0x000f4200}}, 244, 0, true},
        Mode{{{0xd61c7f63, 0xb000002b, 0x0c1e0f16, 0x000f4200}}, 216, 0, true},
        Mode{{{0xd61c7f63, 0xb000002b, 0x0c1e0f16, 0x000f4200}}, 212, 4, true},
        Mode{{{0x201c7b63, 0x30000a2b, 0x041e1f16, 0x000f4200}}, 244, 0, true},
        Mode{{{0x201c7b63, 0x30000a2b, 0x041e0f16, 0x000f4200}}, 244, 0, false},
        Mode{{{0xd61c7f63, 0x3000002b, 0x0c1e0f16, 0x000f4200}}, 216, 0, false},
    };
    for (const auto& mode : modes) {
        for (const u32 source : {mode.boundary - 2, mode.boundary - 1, 255U}) {
            CAPTURE(mode.boundary, mode.high_uniforms, mode.pair, source);
            auto gather = mode.instruction;
            gather.words[0] = (gather.words[0] & 0x00ffffffU) | (source << 24);
            const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
            const auto lowered = Shader::Ampere::LowerToMaxwellWith(
                std::array{gather, exit}, mode.high_uniforms);
            if (mode.pair && source == mode.boundary - 1) {
                REQUIRE_FALSE(static_cast<bool>(lowered));
                REQUIRE(lowered.error == "native texture gather mode");
                REQUIRE(lowered.instructions.empty());
                continue;
            }
            REQUIRE(static_cast<bool>(lowered));
            REQUIRE(lowered.instructions.size() >= 3);
            REQUIRE(((lowered.instructions[0] >> 20) & 255) == source);
            if (mode.pair) {
                REQUIRE(((lowered.instructions[1] >> 20) & 255) ==
                        (source == 255 ? 255U : source + 1));
                REQUIRE(((lowered.instructions[2] >> 20) & 255) == 43);
            } else {
                REQUIRE(((lowered.instructions[1] >> 20) & 255) == 43);
            }
        }
    }
}

TEST_CASE("Ampere signed high-word shifts keep sign extension", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    const std::array<Instruction, 2> code{{
        {{0xff247219, 0x00000025, 0x00011444, 0x000fc600}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[0] >> 8) & 0xff) == 68);
    REQUIRE(((lowered.instructions[0] >> 20) & 0xff) == 37);
    REQUIRE(((lowered.instructions[0] >> 48) & 1) == 1);
}

TEST_CASE("Ampere uniform multiply accepts an immediate addend", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    const std::array<Instruction, 2> code{{
        {{0x040474a4, 0x00000001, 0x0f8e0205, 0x000fe200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE((lowered.instructions[lowered.instructions.size() - 2] & 255) == 220);
}

TEST_CASE("Ampere dual predicate LUTs read original inputs and preserve their guard", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    for (const u32 guard : {0U, 7U, 8U}) {
        const std::array<Instruction, 2> code{{
            {{0x00c8081c | (guard << 12), 0, 0x0010f010, 0x000fc400}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        for (u32 input = 0; input < 4; ++input) {
            std::array<bool, 8> predicates{};
            predicates[0] = input & 1;
            predicates[1] = input & 2;
            predicates[7] = true;
            const auto before = predicates;
            std::array<bool, 256> registers{};
            const auto pred = [&](u32 encoded) { return predicates[encoded & 7] != bool(encoded & 8); };
            for (const u64 word : lowered.instructions) {
                if ((word >> 56) == 0xe3) continue; // EXIT
                if (!pred((word >> 16) & 15)) continue;
                if ((word >> 52) == 0x39a) {
                    // SEL Rd, RZ, -1, P: select zero when P is true.
                    registers[word & 255] = !pred((word >> 39) & 15);
                } else {
                    REQUIRE((word >> 52) == 0x5be);
                    const u32 index = (registers[(word >> 8) & 255] ? 4U : 0U) |
                                      (registers[(word >> 20) & 255] ? 2U : 0U) |
                                      (registers[(word >> 39) & 255] ? 1U : 0U);
                    const bool value = (word >> (28 + index)) & 1;
                    const u32 destination = (word >> 48) & 7;
                    if (destination != 7) predicates[destination] = value;
                }
            }
            const bool active = before[guard & 7] != bool(guard & 8);
            const u32 index = (before[0] ? 4U : 0U) | 2U | (before[1] ? 1U : 0U);
            REQUIRE(predicates[0] == (active ? bool((0x80 >> index) & 1) : before[0]));
            REQUIRE(predicates[1] == (active ? bool((0xc8 >> index) & 1) : before[1]));
        }
    }
}
