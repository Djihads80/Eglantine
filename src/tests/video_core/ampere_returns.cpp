// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere IMMA cannot overwrite any word containing a return link", "[ampere]") {
    for (u32 link : {41U, 42U, 43U}) {
        const std::array<Instruction, 5> code{{
            {{0x00007802U | (link << 16), 0x20, 0xf00, 0}},
            {{0x00007944, 0x10, 0x03c00000, 0}},
            {{0x0000794d, 0, 0x03800000, 0}},
            {{0x08287237, 12, 0x00405cff, 0}},
            {{0x00007950U | (link << 24), 0xffffffb0, 0x03c3ffff, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        CHECK_FALSE(static_cast<bool>(lowered));
        CHECK(lowered.error == "native callee overwrites link register");
    }
}

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

TEST_CASE("Ampere a conditional-exit main loop can precede its called library", "[video_core][ampere]") {
    const std::array<Instruction, 5> code{{
        {{0x00377802, 0x20, 0xf00, 0}}, // R55 = return PC.
        {{0x00007944, 0x20, 0x03c00000, 0}}, // CALL instruction 4.
        {{0x0000094d, 0, 0x03800000, 0}}, // @P0 EXIT.
        {{0x00007947, 0xffffffc0, 0x03ffffff, 0}}, // BRA instruction 0.
        {{0x37007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
    auto conditional_branch = code;
    conditional_branch[3].words[0] = 0x947;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(conditional_branch)));
    auto no_exit = code;
    no_exit[2] = {{0x7918, 0, 0, 0}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(no_exit)));
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

TEST_CASE("Ampere callees can compare into predicates with an R0 link", "[video_core][ampere]") {
    // ISETP's zero destination field is not a write to R0.
    const std::array<Instruction, 5> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x0100720c, 0x00000002, 0x03f06070, 0}},
        {{0x00007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere callees reject a wide multiply over the link", "[video_core][ampere]") {
    // IMAD.WIDE.U32 R54:R55 writes the R55 link.
    const std::array<Instruction, 5> code{{
        {{0x00377802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x01367225, 0x00000002, 0x078e00ff, 0}},
        {{0x37007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native callee overwrites link register");
    REQUIRE(lowered.error_offset == 0x30);
}

TEST_CASE("Ampere callees may clear the link after copying it into the return pair", "[video_core][ampere]") {
    // Captured epilogue (Yoshi, Star Wars Outlaws and others, link R13 or R31):
    // MOV R12, R13; MOV R13, 0x0; RET.REL.NODEC R12. R12:R13 = {link, 0} is the return pair.
    const std::array<Instruction, 7> code{{
        {{0x000d7802, 0x20, 0x00000f00, 0}},         // R13 = return PC.
        {{0x00007944, 0x20, 0x03c00000, 0}},         // CALL instruction 4.
        {{0x0000794d, 0, 0x03800000, 0}},            // EXIT.
        {{0x00007918, 0, 0, 0}},                     // NOP.
        {{0x000c7202, 0x0000000d, 0x00000f00, 0}},   // MOV R12, R13.
        {{0x000d7802, 0x00000000, 0x00000f00, 0}},   // MOV R13, 0x0.
        {{0x0c007950, 0xffffff90, 0x03c3ffff, 0}},   // RET.REL.NODEC R12.
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
    REQUIRE(lowered.instructions.back() == 0xe32000000007000fULL);

    auto imad_clear = code;
    imad_clear[5] = {{0xff0d7424, 0, 0x078e00ff, 0}}; // IMAD.MOV.U32 R13, RZ, RZ, RZ.
    CHECK(static_cast<bool>(LowerToMaxwell(imad_clear)));

    const auto rejected = [](const auto& variant) {
        const auto result = LowerToMaxwell(variant);
        return !result && result.error == "native callee overwrites link register";
    };
    auto cleared_first = code;
    std::swap(cleared_first[4], cleared_first[5]);
    CHECK(rejected(cleared_first));
    auto nonzero = code;
    nonzero[5].words[1] = 1;
    CHECK(rejected(nonzero));
    auto predicated = code;
    predicated[5].words[0] = 0x000d0802; // @P0
    CHECK(rejected(predicated));
    auto other_pair = code;
    other_pair[4].words[0] = 0x000b7202; // MOV R11, R13: R11:R12 is not {link, 0}.
    other_pair[6].words[0] = 0x0b007950;
    CHECK(rejected(other_pair));
    auto entered = code;
    entered[3] = {{0x00007947, 0x10, 0x03800000, 0}}; // BRA instruction 5 skips the copy.
    CHECK(rejected(entered));
}
} // namespace
