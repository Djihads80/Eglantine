// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere guarded ULEA without .HI or a carry out lowers", "[video_core][ampere]") {
    // @!UP5 ULEA UR4, UR6, 0x1, 0x2 and the same on UR5, from Pokemon Pokopia's fullscreen
    // pixel shaders (the black sky).
    for (const Instruction lea : {Instruction{{0x0604d891, 0x1, 0x0f8e103f, 0x000fca00}},
                                  Instruction{{0x0405d891, 0x1, 0x0f8e103f, 0x000fe200}}}) {
        const auto lowered = LowerToMaxwell(std::array{lea, Exit});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        // A carry out (bits 81-83) still needs R253, which holds the swapped-in guard.
        auto carry = lea;
        carry.words[2] &= ~(7U << 17);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{carry, Exit})));
    }
}
} // namespace
