// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

TEST_CASE("Ampere indexed constant loads retain narrow sizes", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction load{{0x02147b82, 0, 0, 0}};
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};

    const auto lowered_load = LowerToMaxwell(std::array{load, exit});
    REQUIRE(static_cast<bool>(lowered_load));
    REQUIRE(lowered_load.instructions.size() == 2);
    REQUIRE((lowered_load.instructions[0] & 0xff) == 20);
    REQUIRE(((lowered_load.instructions[0] >> 8) & 0xff) == 2);

    for (const u32 mode : {0U, 2U}) {
        for (u32 width = 0; width <= 5; ++width) {
            auto variant = load;
            variant.words[2] = (width << 9) | (mode << 14);
            const auto lowered = LowerToMaxwell(std::array{variant, exit});
            REQUIRE(static_cast<bool>(lowered));
            REQUIRE(((lowered.instructions[0] >> 48) & 7) == width);
            REQUIRE(((lowered.instructions[0] >> 44) & 3) == mode);
        }
    }

    auto unsupported = load;
    unsupported.words[2] = 6U << 9;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
    unsupported.words[0] = 0x02157b82;
    unsupported.words[2] = 5U << 9;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
    unsupported.words[2] = 1U << 14;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
}

TEST_CASE("Ampere uniform constant loads retain narrow sizes", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    // Star Fox: ULDC.U16 UR4, c[6][0x20].
    Instruction load{{0x00047ab9, 0x01800800, 0x00000400, 0x000fe200}};
    auto lowered = LowerToMaxwell(std::array{load, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(std::find(lowered.instructions.begin(), lowered.instructions.end(),
                      0xef9200600207ffdcULL) != lowered.instructions.end());
    for (u32 width : {0U, 1U, 2U, 3U, 5U}) {
        load.words[2] = width << 9;
        lowered = LowerToMaxwell(std::array{load, exit});
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(((lowered.instructions[0] >> 48) & 7) == width);
    }
    load.words[2] = 6U << 9;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{load, exit})));
    load.words[2] = 2U << 9;
    load.words[1] |= 1U << 6;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{load, exit})));
}
