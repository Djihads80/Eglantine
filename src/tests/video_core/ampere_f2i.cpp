// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

TEST_CASE("Ampere F2I round up preserves conversion mode", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};

    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 0x00000009,
                                   is_signed ? 0x0021b100U : 0x0020b000U, 0}};
        const auto lowered = LowerToMaxwell(std::array{convert, exit});
        REQUIRE(static_cast<bool>(lowered));
        const auto instruction = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(instruction != lowered.instructions.end());
        REQUIRE((*instruction & 0xff) == 6);
        REQUIRE(((*instruction >> 8) & 3) == 2);
        REQUIRE(((*instruction >> 10) & 3) == 2);
        REQUIRE(((*instruction >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*instruction >> 39) & 3) == 2);
        REQUIRE(((*instruction >> 44) & 1) == static_cast<u64>(is_signed));
    }

    const Instruction unsupported{{0x00067305, 0x00000009, 0x0020b001, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
}

TEST_CASE("Ampere F2I signed byte clamps after conversion", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction convert{{0x00097305, 0x00000005, 0x0021e100, 0}};
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    const auto lowered = LowerToMaxwell(std::array{convert, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 3);

    const u64 f2i = lowered.instructions[0];
    REQUIRE((f2i & 0xff) == 9);
    REQUIRE(((f2i >> 8) & 3) == 1);
    REQUIRE(((f2i >> 10) & 3) == 2);
    REQUIRE(((f2i >> 12) & 1) == 1);
    REQUIRE(((f2i >> 39) & 3) == 3);
    REQUIRE(((f2i >> 44) & 1) == 1);

    const u64 i2i = lowered.instructions[1];
    REQUIRE((i2i & 0xff) == 9);
    REQUIRE(((i2i >> 8) & 3) == 0);
    REQUIRE(((i2i >> 10) & 3) == 1);
    REQUIRE(((i2i >> 12) & 3) == 3);
    REQUIRE(((i2i >> 20) & 0xff) == 9);
    REQUIRE(((i2i >> 50) & 1) == 1);
}
