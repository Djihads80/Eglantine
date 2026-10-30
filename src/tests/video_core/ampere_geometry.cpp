// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
std::vector<Instruction> GeometryProgram() {
    return {{{{0x0000734e, 0, 0, 0}}},          {{{0x00037802, 0x70, 0xf00, 0}}},
            {{{0x00007210, 3, 0x07f1e0ff, 0}}}, {{{0x01017210, 0xff, 0x007fe4ff, 0}}},
            {{{0x00007357, 0, 0, 0}}},          {{{0x0000794d, 0, 0x03800000, 0}}},
            {{{0x00007918, 0, 0, 0}}},          {{{0x00ff7324, 255, 0, 0}}},
            {{{0x00007957, 0, 0, 0}}},          {{{0x0000794d, 0, 0x03800000, 0}}}};
}
u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}
} // namespace

TEST_CASE("Native capture retains ATEXIT finalization", "[ampere-geometry]") {
    const auto code = GeometryProgram();
    Shader::Ampere::ProgramCaptureState capture;
    for (u32 i = 0; i < code.size(); ++i) {
        const auto result = capture.Observe(code[i], i);
        REQUIRE(result == (i + 1 == code.size()
                               ? Shader::Ampere::ProgramCaptureState::Result::Complete
                               : Shader::Ampere::ProgramCaptureState::Result::Continue));
    }
}

TEST_CASE("Native exits branch through real geometry finalization", "[ampere-geometry]") {
    const auto code = GeometryProgram();
    const auto lowered = Shader::Ampere::LowerToMaxwell(
        code, {.code_address = 0x11fffffff0ULL, .single_invocation_geometry = true});
    REQUIRE(lowered);
    REQUIRE(static_cast<u32>(lowered.instructions[0] >> 20) == 0xfffffff0U);
    REQUIRE(static_cast<u32>(lowered.instructions[1] >> 20) == 0x11);
    const u32 branch = lowered.source_instruction_offsets[5];
    const u64 word = lowered.instructions[branch];
    REQUIRE((word >> 48) == 0xe240);
    const s32 relative = static_cast<s32>((word >> 20) & 0xffffff) << 8 >> 8;
    REQUIRE(static_cast<s64>(Address(branch)) + 8 + relative ==
            Address(lowered.source_instruction_offsets[7]));
    const u64 final = lowered.instructions[lowered.source_instruction_offsets[7]];
    REQUIRE((final >> 48) == 0xfbe0);
    REQUIRE(((final >> 40) & 1) == 1); // EndPrimitive, not a skipped OUT.FINAL.
    REQUIRE(((final >> 39) & 1) == 0);
    REQUIRE((lowered.instructions.back() >> 48) == 0xe300);
}

TEST_CASE("Native ATEXIT requires a complete verified epilogue", "[ampere-geometry]") {
    auto code = GeometryProgram();
    code.pop_back();
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    code = GeometryProgram();
    code[8].words[1] = 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    code = GeometryProgram();
    code[1].words[1] = 0x71;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
}

TEST_CASE("Native input entry zero requires a single-invocation proof", "[ampere-geometry]") {
    std::vector<Instruction> code{{{0x00007919, 0, 0x1d00, 0}},
                                  {{0x00007816, 0x7770, 255, 0}},
                                  {{0x01007224, 0, 0x078e02ff, 0}},
                                  {{0x00077923, 0, 0, 0}},
                                  {{0x0000794d, 0, 0x03800000, 0}}};
    REQUIRE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
    auto with_zero_pair = code;
    with_zero_pair.insert(with_zero_pair.begin() + 1, Instruction{{0x00287805, 0, 0x0001ff00, 0}});
    REQUIRE(Shader::Ampere::LowerToMaxwell(with_zero_pair, {.single_invocation_geometry = true}));
    code[1].words[1] = 0x7772;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
}

TEST_CASE("Geometry local-memory uniforms retain real addresses", "[ampere-geometry]") {
    const std::array store{Instruction{{0xff007987, 0x4004, 0x08100c08, 0}},
                           Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto written = Shader::Ampere::LowerToMaxwell(store);
    REQUIRE(written);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE((written.instructions[i] & 255) == 4 + i);
        REQUIRE(((written.instructions[i] >> 8) & 255) == 224);
        REQUIRE(((written.instructions[i] >> 20) & 0xffffff) == 0x40 + i * 4);
    }
    const std::array load{Instruction{{0xff337983, 0x6c08, 0x08100800, 0}},
                          Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto read = Shader::Ampere::LowerToMaxwell(load);
    REQUIRE(read);
    REQUIRE((read.instructions.front() & 255) == 51);
    REQUIRE(((read.instructions.front() >> 8) & 255) == 224);
    REQUIRE(((read.instructions.front() >> 20) & 0xffffff) == 0x6c);
}

TEST_CASE("Geometry wide funnel shift retains both input words", "[ampere-geometry]") {
    for (const u32 shift : {0U, 1U, 16U, 31U, 32U}) {
        const std::array code{Instruction{{0x100f7819, shift, 0x0001020f, 0}},
                              Instruction{{0x794d, 0, 0x03800000, 0}}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(lowered);
        REQUIRE(static_cast<u32>(lowered.instructions.front() >> 20) == shift);
        const u64 word = lowered.instructions[1];
        REQUIRE(((word >> 8) & 255) == 16);
        REQUIRE(((word >> 39) & 255) == 15);
        REQUIRE(((word >> 37) & 3) == 2);
        REQUIRE(((word >> 20) & 255) == 251);
        for (const u32 low : {0U, 0x12345678U, 0xffffffffU})
            for (const u32 high : {0U, 0x87654321U, 0xffffffffU}) {
                const u32 result = static_cast<u32>(((u64{high} << 32 | low) << shift) >> 32);
                const u32 separate = shift == 0    ? high
                                     : shift == 32 ? low
                                                   : (high << shift) | (low >> (32 - shift));
                REQUIRE(result == separate);
            }
    }
}

TEST_CASE("Geometry half-word local stores preserve adjacent bytes", "[ampere-geometry]") {
    const std::array code{Instruction{{0xff007987, 0x9488, 0x08100408, 0}},
                          Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(lowered);
    const u64 word = lowered.instructions.front();
    REQUIRE((word & 255) == 136);
    REQUIRE(((word >> 8) & 255) == 224);
    REQUIRE(((word >> 20) & 0xffffff) == 0x94);
    REQUIRE(((word >> 48) & 7) == 2);
}

TEST_CASE("Geometry indexed vector loads preserve aliased addresses", "[ampere-geometry]") {
    for (u32 address : {26U, 43U}) {
        const std::array code{Instruction{{0x001a7983 | (address << 24), 0xc800, 0x00100a00, 0}},
                              Instruction{{0x794d, 0, 0x03800000, 0}}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(lowered);
        const u32 start = address == 26 ? 1 : 0;
        if (start) {
            REQUIRE((lowered.instructions[0] & 255) == 251);
            REQUIRE(((lowered.instructions[0] >> 20) & 255) == address);
        }
        for (u32 i = 0; i < 2; ++i) {
            const auto word = lowered.instructions[start + i];
            REQUIRE((word & 255) == 26 + i);
            REQUIRE(((word >> 8) & 255) == (start ? 251 : address));
            REQUIRE(((word >> 20) & 0xffffff) == 0xc8 + i * 4);
        }
    }
}
