// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
// Captured from Kirby Air Riders Global Test Ride, fragment program.
// nvdisasm SM86: TEX.SCR.NODEP R4, R6, R4, R5, UR4, 0x0, 2D
constexpr Shader::Ampere::Instruction UniformSample{{0x04067f60, 0x30000405, 0x0c1e0f04, 0}};
// nvdisasm SM86: TEX.SCR.B.NODEP R10, R22, R16, R10, 2D
constexpr Shader::Ampere::Instruction PerLaneSample{{0x10163361, 0x3800000a, 0x041e0f0a, 0}};

Shader::Ampere::LoweredProgram Lower(Shader::Ampere::Instruction instruction) {
    const std::array program{instruction,
                             Shader::Ampere::Instruction{{0x0000794d, 0, 0x03800000, 0}}};
    return Shader::Ampere::LowerToMaxwell(program);
}

u32 Field(u64 word, u32 shift) {
    return static_cast<u32>((word >> shift) & 0xff);
}
} // namespace

TEST_CASE("Ampere bindless sample keeps the rendering-validated destination order", "[ampere-sample]") {
    // The title screen of the shipped game renders only with this field order: components 0/1 go
    // to the destination at bits 16..23 and components 2/3 to the pair at bits 64..71. Swapping
    // them produced a magenta title screen, so the native operand print order is not the result
    // order here.
    const auto lowered = Lower(UniformSample);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 9);
    const auto& words = lowered.instructions;
    REQUIRE(Field(words[0], 20) == 4);   // u
    REQUIRE(Field(words[1], 20) == 5);   // v
    REQUIRE(Field(words[2], 20) == 220); // UR4 lives in R220
    const u64 sample = words[3];
    REQUIRE(((sample >> 52) & 0xffc) == 0xde8); // sparse-predicate bits are fixed to PT
    REQUIRE(Field(sample, 0) == 250);
    REQUIRE(((sample >> 28) & 7) == 2);    // 2D
    REQUIRE(((sample >> 31) & 0xf) == 15); // all four channels
    REQUIRE(Field(words[4], 0) == 6);
    REQUIRE(Field(words[5], 0) == 7);
    REQUIRE(Field(words[6], 0) == 4);
    REQUIRE(Field(words[7], 0) == 5);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(Field(words[4 + i], 20) == 250 + i);
    }
    // A single-channel sample writes the primary destination only.
    auto narrow = UniformSample;
    narrow.words[2] = (narrow.words[2] & ~(0xfU << 8)) | (1U << 8);
    const auto single = Lower(narrow);
    REQUIRE(single);
    REQUIRE(single.instructions.size() == 6);
    REQUIRE(Field(single.instructions[4], 0) == 6);
    REQUIRE((single.instructions[5] >> 48) == 0xe300); // EXIT
}

TEST_CASE("Ampere per-lane bindless sample uses the descriptor register", "[ampere-sample]") {
    const auto lowered = Lower(PerLaneSample);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 9);
    const auto& words = lowered.instructions;
    REQUIRE(Field(words[0], 20) == 22); // u
    REQUIRE(Field(words[1], 20) == 16); // v
    REQUIRE(Field(words[2], 20) == 10); // per-lane descriptor
    const u64 sample = words[3];
    REQUIRE(((sample >> 52) & 0xffc) == 0xde8);
    REQUIRE(Field(sample, 20) == 246); // handle register
    REQUIRE(((sample >> 28) & 7) == 2);
    REQUIRE(((sample >> 31) & 0xf) == 15);
    REQUIRE(((sample >> 49) & 1) == 1); // NODEP is conveyed to Maxwell
    // The per-lane form has a single printed destination; four channels occupy four registers.
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(Field(words[4 + i], 0) == 10 + i);
        REQUIRE(Field(words[4 + i], 20) == 250 + i);
    }
}

TEST_CASE("Ampere per-lane bindless sample rejects unproven modes", "[ampere-sample]") {
    auto instruction = PerLaneSample;
    instruction.words[2] = (instruction.words[2] & ~(0xfU << 8)) | (0x7U << 8); // three channels
    REQUIRE(Lower(instruction));
    instruction = PerLaneSample;
    instruction.words[2] = (instruction.words[2] & ~(0xfU << 8)) | (0U << 8); // no channels
    REQUIRE_FALSE(Lower(instruction));
    instruction = PerLaneSample;
    instruction.words[2] = (instruction.words[2] & ~(0xfU << 8)) | (0x3U << 8);
    instruction.words[2] = (instruction.words[2] & ~255U) | 11U; // odd split destination
    REQUIRE_FALSE(Lower(instruction));
    instruction = PerLaneSample;
    instruction.words[2] = (instruction.words[2] & ~255U) | 255U; // discarded result
    const auto discarded = Lower(instruction);
    REQUIRE(discarded);
    REQUIRE(discarded.instructions.size() == 5);
    instruction = PerLaneSample;
    instruction.words[1] = (instruction.words[1] & ~255U) | 255U; // descriptor in RZ
    REQUIRE_FALSE(Lower(instruction));
    instruction = PerLaneSample;
    instruction.words[1] |= 1U << 24; // non-2D dimension encoding
    REQUIRE_FALSE(Lower(instruction));
    instruction = PerLaneSample;
    instruction.words[2] |= 1U << 16; // 16-bit packed result format
    REQUIRE_FALSE(Lower(instruction));
}
