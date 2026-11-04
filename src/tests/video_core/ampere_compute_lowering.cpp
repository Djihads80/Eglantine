// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere LEA.HI with a constant base and no carry lowers", "[video_core][ampere]") {
    // LEA.HI R1, R17, c[0][0x0], RZ, 0x1e from Pokemon Pokopia's compute shaders.
    const std::array<Instruction, 2> code{{{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto& out = lowered.instructions;
    // R250 = R17 >> 2 (32 - 0x1e, unsigned), then R1 = R250 + the constant.
    REQUIRE(std::find(out.begin(), out.end(), 0x38280000002711faULL) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1000000fb7fa01ULL) != out.end());
}

TEST_CASE("Ampere LEA.HI with a constant base still rejects a carry out", "[video_core][ampere]") {
    // The same LEA.HI writing a carry out to P0 has no lowering yet.
    Instruction carry{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}};
    carry.words[2] &= ~(7U << 17);
    const std::array<Instruction, 2> code{{carry, Exit}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere LEA with a constant base and no carry out lowers to ISCADD", "[video_core][ampere]") {
    // LEA R0, R0, c[0][0x2e0]-style constant, 0x8 from Pokemon Pokopia's album compute shaders.
    const std::array<Instruction, 2> code{{{{0x00007a11, 0x01002e00, 0x078e40ff, 0x000fe200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // ISCADD R0, R0, R251, 8 with the constant loaded into R251, and no carry compare.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1804000fb70000ULL) != out.end());
    REQUIRE(std::none_of(out.begin(), out.end(), [](u64 i) { return (i >> 48) == 0x5b60; }));
}

TEST_CASE("Ampere SHF.L.U32.HI immediate without .W lowers to a clamping SHL", "[video_core][ampere]") {
    // SHF.L.U32.HI R16, RZ, R14, 0xff from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> code{{{{0xff107419, 0x000000ff, 0x0001060e, 0x080fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // SHL R16, R250, R14 without .W (bit 39), so a shift of 32 or more gives zero.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c48000000e7fa10ULL) != out.end());
}

TEST_CASE("Ampere guarded UIADD3 lowers to a guarded IADD32I or IADD", "[video_core][ampere]") {
    // @!UP0 UIADD3 UR6, UR14, 0x1c, URZ from Pokemon Pokopia's photo compute shaders.
    // @UP0 UIADD3 UR6, UR6, UR7, URZ from the same shaders (UR second source).
    struct Case { u32 guarded, plain, guard; };
    // @!UP1 UIADD3 UR6, UR9, UR8, UR6 (a third source: two guarded adds).
    struct Words { u32 b, w2, w3; };
    const auto words = [](u32 guarded) {
        return guarded == 0x0e068890 ? Words{0x1c, 0x0fffe03f, 0x000fcc00}
             : guarded == 0x06060290 ? Words{0x7, 0x0fffe03f, 0x000fc800}
                                     : Words{0x8, 0x0fffe006, 0x000fc800};
    };
    for (const Case c : {Case{0x0e068890, 0x0e067890, 0xe}, Case{0x06060290, 0x06067290, 0x6},
                         Case{0x09069290, 0x09067290, 0xd}}) {
        const Words w = words(c.guarded);
        const std::array<Instruction, 2> guarded{{{{c.guarded, w.b, w.w2, w.w3}}, Exit}};
        const std::array<Instruction, 2> plain{{{{c.plain, w.b, w.w2, w.w3}}, Exit}};
        const auto lowered = LowerToMaxwell(guarded);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto reference = LowerToMaxwell(plain);
        REQUIRE(static_cast<bool>(reference));
        // The same add as the unguarded one, predicated on P6 (negated for @!UP0) where UP0 is swapped in.
        // Every add of the unguarded lowering appears again, predicated on the swapped-in guard.
        const auto& out = lowered.instructions;
        for (const u64 add : reference.instructions) {
            if ((add >> 52) == 0xe30) continue; // EXIT
            const u64 guarded_add = (add & ~(u64{0xf} << 16)) | (u64{c.guard} << 16);
            REQUIRE(std::find(out.begin(), out.end(), guarded_add) != out.end());
        }
    }
}

TEST_CASE("Ampere DEPBAR.LE with any scoreboard and wait list is a no-op", "[video_core][ampere]") {
    // DEPBAR.LE SB5, 0x1 and DEPBAR.LE SB0, 0x0, {3,2,1} (Pokemon Pokopia's photo compute shaders).
    for (const u32 word : {0x0000d040U, 0x0000800eU}) {
        const std::array<Instruction, 2> code{{{{0x0000791a, word, 0, 0x000fce00}}, Exit}};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == LowerToMaxwell(std::array{Exit}).instructions.size());
    }
}

TEST_CASE("Ampere guarded USHF.R.U32.HI lowers to a guarded SHR", "[video_core][ampere]") {
    // @UP0 USHF.R.U32.HI UR7, URZ, 0x2, UR6 from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> guarded{{{{0x3f070899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const std::array<Instruction, 2> plain{{{{0x3f077899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const auto lowered = LowerToMaxwell(guarded);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto reference = LowerToMaxwell(plain);
    REQUIRE(static_cast<bool>(reference));
    const u64 shift = reference.instructions.front();
    const u64 guarded_shift = (shift & ~(u64{0xf} << 16)) | (u64{6} << 16);
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), guarded_shift) != out.end());
}
} // namespace
