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

u64 FindMaxwellIsberd(const Shader::Ampere::LoweredProgram& lowered) {
    for (const u64 instruction : lowered.instructions) {
        if ((instruction >> 51) == (0xefd0000000000000ULL >> 51)) return instruction;
    }
    return 0;
}

TEST_CASE("Ampere skewed ISBERD keeps the immediate offset", "[video_core][ampere]") {
    // isberd.attr.skew R4, [RZ], B32
    const std::array<Instruction, 2> code{{
        {{0xff047923, 0, 0x00007800, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 isberd = FindMaxwellIsberd(lowered);
    REQUIRE(isberd != 0);
    REQUIRE((isberd & 0xff) == 4);
    REQUIRE(((isberd >> 8) & 0xff) == 246);  // lane offset computed into the scratch register
    REQUIRE(((isberd >> 24) & 0xff) == 0);   // imm 0 and skew clear
    REQUIRE(((isberd >> 33) & 3) == 3);      // attribute
    REQUIRE(((isberd >> 36) & 0xf) == 2);    // U32
}

TEST_CASE("Ampere ISBERD output reads are rejected", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0xff047923, 0, 0x0000f800, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native stage buffer output read");
}

TEST_CASE("Ampere ISBEWR is dropped and reported", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x05000927, 0x00001004, 0x0000b800, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.dropped_stage_buffer_writes);
}
} // namespace
