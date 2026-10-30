// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
Shader::Ampere::Instruction Gather(u32 mode) {
    // Synthetic TLD4.SCR.R: R20/R22 <- coordinates R4/R8, binding 2, descriptor 0.
    return {{{0x04167b63, 0x30800008, 0x000e0f14 | (mode << 12), 0}}};
}

Shader::Ampere::LoweredProgram Lower(Shader::Ampere::Instruction instruction) {
    const std::array program{instruction,
        Shader::Ampere::Instruction{{0x0000794d, 0, 0x03800000, 0}}};
    return Shader::Ampere::LowerToMaxwell(program);
}
} // namespace

TEST_CASE("Ampere gather preserves packed offset operands", "[ampere-gather]") {
    for (u32 mode = 0; mode < 3; ++mode) {
        CAPTURE(mode);
        const auto lowered = Lower(Gather(mode));
        REQUIRE(lowered);
        REQUIRE(lowered.required_texture_buffer == 2);
        const u32 operands = mode == 0 ? 2 : mode == 1 ? 3 : 4;
        REQUIRE(lowered.instructions.size() == operands + 6);
        const auto& words = lowered.instructions;
        const auto reg = [&](u32 i) { return (words[i] >> 20) & 255; };
        REQUIRE(reg(0) == 4);
        REQUIRE(reg(1) == (mode == 0 ? 8 : 5));
        if (mode != 0) REQUIRE(reg(2) == 8);
        if (mode == 2) REQUIRE(reg(3) == 9);
        const u64 gather = words[operands];
        REQUIRE(((gather >> 54) & 3) == mode);
        REQUIRE(((gather >> 20) & 255) == (mode == 0 ? 255 : 246));
        REQUIRE((gather & 255) == 250);
        REQUIRE((words[operands + 1] & 255) == 22);
        REQUIRE((words[operands + 3] & 255) == 20);
    }
}

TEST_CASE("Ampere gather rejects unsupported modes and scratch aliases", "[ampere-gather]") {
    REQUIRE_FALSE(Lower(Gather(3)));
    auto instruction = Gather(2);
    instruction.words[2] |= 1U << 14; // Depth comparison remains unsupported.
    REQUIRE_FALSE(Lower(instruction));
    instruction = Gather(2);
    instruction.words[1] = (instruction.words[1] & ~255U) | 245U;
    REQUIRE_FALSE(Lower(instruction));
    instruction = Gather(0);
    instruction.words[0] = (instruction.words[0] & ~(255U << 16)) | (244U << 16);
    instruction.words[2] = (instruction.words[2] & ~255U) | 255U;
    REQUIRE_FALSE(Lower(instruction)); // Four unsplit outputs would overwrite scratch.
}

TEST_CASE("Ampere match all supports a single odd-numbered source register", "[ampere-match]") {
    // nvdisasm SM86: MATCH.ALL P0, RZ, R5 (32-bit, not a register pair).
    const auto narrow = Lower({{{0x05ff73a1, 0, 0, 0x000e2400}}});
    REQUIRE(narrow);
    const auto wide = Lower({{{0x04ff73a1, 0, 0x200, 0x000e2400}}});
    REQUIRE(wide);
    REQUIRE(wide.instructions.size() == narrow.instructions.size() + 2);
    u32 shuffles{}, compares{}, votes{};
    for (const u64 instruction : narrow.instructions) {
        const u64 op = instruction >> 48;
        if ((op & 0xfff8) == 0xef10) {
            ++shuffles;
            REQUIRE(((instruction >> 8) & 255) == 5);
        }
        if ((op & 0xfff0) == 0x5b60) {
            ++compares;
            REQUIRE(((instruction >> 8) & 255) == 5);
            REQUIRE(((instruction >> 39) & 7) == 7);
        }
        if ((op & 0xfff8) == 0x50d8) ++votes;
    }
    REQUIRE(shuffles == 1);
    REQUIRE(compares == 1);
    REQUIRE(votes == 2); // Active-lane mask and final all-equal vote.
    REQUIRE_FALSE(Lower({{{0x05ff73a1, 0, 0x200, 0}}})); // Odd 64-bit source.
    REQUIRE_FALSE(Lower({{{0x05fe73a1, 0, 0, 0}}})); // Returned lane masks unsupported.
    REQUIRE_FALSE(Lower({{{0x05ff73a1, 0, 0x400, 0}}})); // Other match modes unsupported.
}

TEST_CASE("Ampere shared atomic OR retains its predicate and operands", "[ampere-atomic]") {
    // nvdisasm SM86: @!P0 ATOMS.OR RZ, [R5], R3.
    const auto lowered = Lower({{{0x05ff838c, 3, 0x03000000, 0x0011e200}}});
    REQUIRE(lowered);
    const u64 instruction = lowered.instructions.front();
    REQUIRE(((instruction >> 52) & 15) == 6); // Maxwell shared OR, not ADD.
    REQUIRE((instruction & 255) == 255);
    REQUIRE(((instruction >> 8) & 255) == 5);
    REQUIRE(((instruction >> 16) & 15) == 8);
    REQUIRE(((instruction >> 20) & 255) == 3);
    REQUIRE(((instruction >> 30) & 0x3fffff) == 0);
    const auto add = Lower({{{0x05ff838c, 3, 0, 0}}});
    REQUIRE(add);
    REQUIRE(((add.instructions.front() >> 52) & 15) == 0);
    REQUIRE_FALSE(Lower({{{0x05ff838c, 3, 0x03000001, 0}}}));
    REQUIRE_FALSE(Lower({{{0x05ff838c, 0x103, 0x03000000, 0}}})); // Unaligned offset.
}

TEST_CASE("Ampere warp OR reduces all active lanes into its uniform register", "[ampere-redux]") {
    // nvdisasm SM86: REDUX.OR UR6, R3.
    const auto lowered = Lower({{{0x030673c4, 0, 0x4000, 0x000ea200}}});
    REQUIRE(lowered);
    u32 shuffles{}, reductions{};
    for (const u64 instruction : lowered.instructions) {
        if (((instruction >> 48) & 0xfff8) == 0xef10) {
            REQUIRE(((instruction >> 8) & 255) == 3);
            REQUIRE(((instruction >> 20) & 31) == shuffles);
            ++shuffles;
        }
        if (((instruction >> 48) & 0xfff8) == 0x5c40 && ((instruction >> 41) & 3) == 1) {
            ++reductions;
            REQUIRE((instruction & 255) == 222); // UR6's emulated register.
            REQUIRE(((instruction >> 8) & 255) == 222);
            REQUIRE(((instruction >> 20) & 255) == 251);
        }
    }
    REQUIRE(shuffles == 32);
    REQUIRE(reductions == 32);
    REQUIRE_FALSE(Lower({{{0x030673c4, 0, 0x4001, 0}}}));
}

TEST_CASE("Ampere W-scale reads retain register identity and destination", "[ampere-wscale]") {
    for (u32 special : {30U, 31U}) {
        auto instruction = Shader::Ampere::Instruction{{0x000479c3, 0, special << 8, 0x000e7000}};
        const auto lowered = Lower(instruction);
        REQUIRE(lowered);
        REQUIRE((lowered.instructions.front() & 255) == 220);
        REQUIRE(((lowered.instructions.front() >> 20) & 255) == special);
        instruction.words[1] = 1;
        REQUIRE_FALSE(Lower(instruction));
    }
}

TEST_CASE("Ampere segmented constant loads preserve addressing fields", "[ampere-ldc]") {
    // Captured Kirby LDC.64.IS R6, c[0][R0+8].
    auto instruction = Shader::Ampere::Instruction{{0x00067b82, 0x200, 0x8a00, 0x000e7000}};
    const auto lowered = Lower(instruction);
    REQUIRE(lowered);
    const u64 word = lowered.instructions.front();
    REQUIRE((word & 255) == 6);
    REQUIRE(((word >> 8) & 255) == 0);
    REQUIRE(((word >> 20) & 0xffff) == 8);
    REQUIRE(((word >> 36) & 31) == 0);
    REQUIRE(((word >> 44) & 3) == 2);
    REQUIRE(((word >> 48) & 7) == 5);
    instruction.words[2] = 0x4a00; // IL remains unsupported.
    REQUIRE_FALSE(Lower(instruction));
    instruction.words[2] = 0xca00; // ISL remains unsupported.
    REQUIRE_FALSE(Lower(instruction));
}

TEST_CASE("Ampere clip state updates remain explicit failures", "[ampere-csm]") {
    for (const auto instruction : {
             Shader::Ampere::Instruction{{0x0000780d, 0xc4321111, 0x03fe0000, 0x000fc800}},
             Shader::Ampere::Instruction{{0x0000780d, 0x80000000, 0x03f06000, 0x000fda00}}}) {
        const auto lowered = Lower(instruction);
        REQUIRE_FALSE(lowered);
        REQUIRE(lowered.error == "native clip state machine (CSMTEST)");
    }
}

TEST_CASE("Ampere uniform LEA preserves arithmetic and rejects carry modes", "[ampere-lea]") {
    auto instruction = Shader::Ampere::Instruction{{0x3d3a7c11, 4, 0x0f8e08ff, 0x000fe200}};
    const auto lowered = Lower(instruction);
    REQUIRE(lowered);
    const u64 word = lowered.instructions.front();
    REQUIRE((word & 255) == 58);
    REQUIRE(((word >> 8) & 255) == 61);
    REQUIRE(((word >> 20) & 255) == 220);
    REQUIRE(((word >> 39) & 31) == 1);
    for (u32 x : {0U, 1U, 0x7fffffffU, 0xffffffffU})
        for (u32 y : {0U, 1U, 0x80000000U, 0xffffffffU})
            REQUIRE(static_cast<u32>((u64{x} << ((word >> 39) & 31)) + y) == (x * 2 + y));
    instruction.words[2] ^= 1U << 16; // HI requires separate semantics.
    REQUIRE_FALSE(Lower(instruction));
    instruction.words[2] = 0x0f8e08ff;
    instruction.words[1] = 28; // Outside the supported uniform bank.
    REQUIRE_FALSE(Lower(instruction));
}

TEST_CASE("Ampere wide local zero stores write four consecutive words", "[ampere-local]") {
    const auto lowered = Lower(Shader::Ampere::Instruction{{0xff007387, 0x10ff, 0x00100c00, 0x000fe800}});
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 5);
    for (u32 i = 0; i < 4; ++i) {
        const auto word = lowered.instructions[i];
        REQUIRE((word & 255) == 255);
        REQUIRE(((word >> 8) & 255) == 255);
        REQUIRE(((word >> 20) & 0xffffff) == 16 + i * 4);
        REQUIRE(((word >> 48) & 7) == 4);
    }
    REQUIRE_FALSE(Lower(Shader::Ampere::Instruction{{0xff007387, 0x4ff, 0x00100c00, 0x000fe800}}));
}

TEST_CASE("Ampere uniform LEA stores carry separately from GPR predicates", "[ampere-lea]") {
    const auto lowered = Lower(Shader::Ampere::Instruction{{0x04047291, 6, 0x0f80103f, 0x001fc800}});
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    REQUIRE(words.size() == 7);
    REQUIRE((words[0] & 255) == 253); // Save P6.
    REQUIRE(((words[0] >> 20) & 0x7f) == 64);
    REQUIRE(((words[1] >> 39) & 31) == 2);
    REQUIRE(((words[1] >> 8) & 255) == 220);
    REQUIRE(((words[1] >> 20) & 255) == 222);
    REQUIRE(((words[2] >> 3) & 7) == 6);
    REQUIRE((words[3] & 255) == 239); // Store UP0.
    REQUIRE(((words[4] >> 8) & 255) == 253); // Restore P6.
    REQUIRE((words[5] & 255) == 220);
}

TEST_CASE("Ampere local vectors preserve each register and byte offset", "[ampere-local]") {
    for (const bool store : {false, true}) {
        for (const u32 width : {5U, 6U}) {
            Shader::Ampere::Instruction instruction{{store ? 0xff007387U : 0xff107983U,
                store ? 0x1010U : 0x1000U,
                (store ? 0x00100000U : 0x00300000U) | (width << 9), 0x000fe800}};
            const auto lowered = Lower(instruction);
            REQUIRE(lowered);
            const u32 count = 1U << (width - 4);
            REQUIRE(lowered.instructions.size() == count + 1);
            for (u32 i = 0; i < count; ++i) {
                const auto word = lowered.instructions[i];
                REQUIRE((word & 255) == 16 + i);
                REQUIRE(((word >> 20) & 0xffffff) == 16 + i * 4);
            }
            instruction.words[1] += 4U << 8;
            REQUIRE_FALSE(Lower(instruction));
        }
    }
}

TEST_CASE("Ampere indexed local loads retain the original address register", "[ampere-local]") {
    for (const u32 cache : {0x00100800U, 0x00300800U}) {
        const auto lowered = Lower(Shader::Ampere::Instruction{{0x04047983, 0, cache, 0x000f6200}});
        REQUIRE(lowered);
        const u64 word = lowered.instructions.front();
        REQUIRE((word & 255) == 4);
        REQUIRE(((word >> 8) & 255) == 4);
        REQUIRE(((word >> 20) & 0xffffff) == 0);
    }
}

TEST_CASE("Ampere local vector loads accept the captured default cache mode", "[ampere-local]") {
    const auto lowered = Lower(Shader::Ampere::Instruction{{0xff047983, 0, 0x00100c00, 0x000ea200}});
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 5);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE((lowered.instructions[i] & 255) == 4 + i);
        REQUIRE(((lowered.instructions[i] >> 20) & 0xffffff) == 4 * i);
    }
}

TEST_CASE("Ampere constant clip acceptance is proven from all seven predicates", "[ampere-csm]") {
    std::vector<Shader::Ampere::Instruction> code{
        {{0x000479c3, 0, 0x1e00, 0x000e7000}},
        {{0x000579c3, 0, 0x1f00, 0x000e7000}}};
    for (u32 predicate = 0; predicate < 7; ++predicate) {
        if (predicate == 6)
            code.push_back({{0xff00780b, 0x3f800000, 0x03f1c000 | (predicate << 17), 0x000e7000}});
        else
            code.push_back({{0xff007c0b, predicate < 4 ? 4U : 5U,
                             0x0bf1c000 | (predicate << 17), 0x000e7000}});
    }
    code.push_back({{0x0000780d, 0xc4321111, 0x03fe0000, 0x000fc800}});
    code.push_back({{0x0000780d, 0x80000000, 0x03f06000, 0x000fda00}});
    code.push_back({{0x0000794d, 0, 0x03800000, 0}});
    REQUIRE(Shader::Ampere::LowerToMaxwell(code));
    auto changed = code;
    changed[8].words[1] = 0x7fc00000; // NaN: unordered comparison is true.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(changed));
    changed = code;
    changed[8].words[1] = 0xbf800000; // Negative factor: comparison is true.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(changed));
    changed = code;
    changed[2].words[1] = 0; // UR0 has no proven positive value.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(changed));
    changed = code;
    changed.erase(changed.begin() + 2); // Missing P0 definition.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(changed));
    changed = code;
    changed[9].words[1] ^= 1U << 30; // Other reset/accumulate modes remain unsupported.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(changed));
}

TEST_CASE("Ampere bindless sampling uses the actual uniform handle", "[ampere-bindless]") {
    auto instruction = Shader::Ampere::Instruction{{0x041c7f60, 0x30000605, 0x0c1e01ff, 0x100f4400}};
    const auto lowered = Lower(instruction);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    REQUIRE(words.size() == 6);
    REQUIRE(((words[0] >> 20) & 255) == 4);
    REQUIRE(((words[1] >> 20) & 255) == 5);
    REQUIRE((words[2] & 255) == 246);
    REQUIRE(((words[2] >> 20) & 255) == 222); // UR6.
    REQUIRE(((words[3] >> 20) & 255) == 246); // Bindless metadata starts with the handle.
    REQUIRE(((words[3] >> 28) & 7) == 2); // 2D.
    REQUIRE(((words[3] >> 31) & 15) == 1);
    REQUIRE((words[4] & 255) == 28);
    instruction.words[2] ^= 1U << 14; // Depth comparison is not this form.
    REQUIRE_FALSE(Lower(instruction));
    instruction.words[2] = 0x0c1e01ff;
    instruction.words[1] = 0x30001f05; // Unknown uniform bank entry.
    REQUIRE_FALSE(Lower(instruction));
}

TEST_CASE("Ampere bindless zero-LOD sampling preserves split RGBA outputs", "[ampere-bindless]") {
    auto instruction = Shader::Ampere::Instruction{{0x02007f60, 0x30000403, 0x0c9e0f02, 0x101f4200}};
    const auto lowered = Lower(instruction);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    REQUIRE(words.size() == 9);
    REQUIRE(((words[0] >> 20) & 255) == 2);
    REQUIRE(((words[1] >> 20) & 255) == 3);
    REQUIRE(((words[2] >> 20) & 255) == 220);
    REQUIRE(((words[3] >> 31) & 15) == 15);
    REQUIRE(((words[3] >> 37) & 7) == 1);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE((words[4 + i] & 255) == i);
        REQUIRE(((words[4 + i] >> 20) & 255) == 250 + i);
    }
    instruction.words[2] = 0x0c9e0fff;
    REQUIRE_FALSE(Lower(instruction));
}
