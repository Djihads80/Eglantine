// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere returns do not write the link register", "[video_core][ampere]") {
    for (const u32 link : {0U, 55U}) {
        const std::array<Instruction, 4> code{{
            {{0x00007802U | (link << 16), 0x20, 0x00000f00, 0}},
            {{0x00007944, 0x10, 0x03c00000, 0}},
            {{0x0000794d, 0, 0x03800000, 0}},
            {{0x00007950U | (link << 24), 0xffffffc0, 0x03c3ffff, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.source_size == sizeof(code));
        REQUIRE(lowered.instructions.back() == 0xe32000000007000fULL);
    }
}

TEST_CASE("Ampere returns can read a copy of the R0 link", "[video_core][ampere]") {
    const std::array<Instruction, 5> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00017202, 0, 0x00000f00, 0}},
        {{0x01007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere callees still reject writes to the link", "[video_core][ampere]") {
    const std::array<Instruction, 5> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x00007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native callee overwrites link register");
    REQUIRE(lowered.error_offset == 0x30);
}
} // namespace
