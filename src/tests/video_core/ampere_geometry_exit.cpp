// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/geometry_exit.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::AnalyzeGeometryExit;
using Shader::Ampere::Instruction;

constexpr Instruction ImadCarry{{0xff017224, 0xff, 0x000e0401, 0x000fc800}};
constexpr Instruction Iadd3Carry{{0x01017210, 0xff, 0x007fe4ff, 0x000fc800}};
constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};

constexpr std::array<Instruction, 16> Program{{
    {{0x0000734e, 0, 0, 0x000fe400}},
    {{0x00037802, 0xd0, 0x00000f00, 0x000fc800}},
    {{0x00007210, 3, 0x07f1e0ff, 0x000fca00}},
    ImadCarry,
    {{0x00007357, 0, 0, 0x0001e400}},
    {{0x00007919, 0, 0x00001d00, 0x001e2200}},
    {{0x00007945, 0x30, 0x03800000, 0x000fe200}},
    {{0x00017816, 0x7772, 0x000000ff, 0x041fe400}},
    {{0x0100720c, 0xff, 0x03f03070, 0x040fe200}},
    {{0x00037802, 5, 0x00000f00, 0x000fc800}},
    {{0x00007941, 0, 0x03800000, 0x000fea00}},
    {{0x0000094d, 0, 0x03800000, 0x000fea00}},
    {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    {{0x00ff7324, 0xff, 0, 0x0001e200}},
    {{0x00007957, 0, 0, 0x000fe200}},
    {{0x0000794d, 0, 0x03800000, 0x001fea00}},
}};

template <size_t N>
void RequireAccepted(const std::array<Instruction, N>& code) {
    const auto envelope = AnalyzeGeometryExit(code);
    INFO(envelope.error << " at " << envelope.error_offset);
    REQUIRE(static_cast<bool>(envelope));
    REQUIRE(envelope.handler == 0xd0);
}

template <size_t N>
void RequireRejected(const std::array<Instruction, N>& code, std::string_view error, u32 offset) {
    const auto envelope = AnalyzeGeometryExit(code);
    REQUIRE(envelope.error == error);
    REQUIRE(envelope.error_offset == offset);
    REQUIRE(envelope.exits.empty());
    REQUIRE(envelope.handler == 0);
}

TEST_CASE("Ampere geometry exit envelope routes body exits to the handler", "[video_core][ampere]") {
    for (const Instruction& carry : {ImadCarry, Iadd3Carry}) {
        auto code = Program;
        code[3] = carry;
        const auto envelope = AnalyzeGeometryExit(code);
        REQUIRE(static_cast<bool>(envelope));
        REQUIRE(envelope.handler == 0xd0);
        REQUIRE(envelope.exits.size() == 2);
        REQUIRE(envelope.exits[0].offset == 0xb0);
        REQUIRE(envelope.exits[0].predicate == 0);
        REQUIRE(envelope.exits[1].offset == 0xc0);
        REQUIRE(envelope.exits[1].predicate == 7);
    }
}

TEST_CASE("Ampere geometry exit envelope requires the exact prologue and handler",
          "[video_core][ampere]") {
    for (const size_t index : std::array<size_t, 8>{0, 1, 2, 3, 4, 13, 14, 15}) {
        for (u32 word = 0; word < 4; ++word) {
            for (u32 bit = 0; bit < 32; ++bit) {
                auto code = Program;
                code[index].words[word] ^= 1U << bit;
                const bool scheduling = word == 3 && bit >= 9 && bit <= 29;
                INFO("instruction " << index << " word " << word << " bit " << bit);
                REQUIRE(static_cast<bool>(AnalyzeGeometryExit(code)) == scheduling);
            }
        }
    }
}

TEST_CASE("Ampere geometry exit envelope bounds the captured program", "[video_core][ampere]") {
    const std::span<const Instruction> program{Program};
    for (const size_t size : std::array<size_t, 5>{0, 5, 8, 13, 15})
        REQUIRE(!AnalyzeGeometryExit(program.first(size)));
    std::array<Instruction, 17> longer{};
    std::copy(Program.begin(), Program.end(), longer.begin());
    longer[16] = Program[15];
    RequireRejected(longer, "native exit handler location", 0x10);
    for (const u32 target : {0x50U, 0x60U, 0xc0U, 0xd8U, 0xe0U}) {
        auto code = Program;
        code[1].words[1] = target;
        RequireRejected(code, "native exit handler location", 0x10);
    }
}

TEST_CASE("Ampere geometry exit handler is not reached by fallthrough", "[video_core][ampere]") {
    auto code = Program;
    code[12].words[0] = 0x0000094d;
    RequireRejected(code, "native exit handler reached by fallthrough", 0xc0);
    code[12] = Nop;
    RequireRejected(code, "native exit handler reached by fallthrough", 0xc0);
}

TEST_CASE("Ampere geometry exit setup results must be dead before the body reads them",
          "[video_core][ampere]") {
    constexpr std::string_view reads = "native body reads exit setup state";
    constexpr std::string_view unmodeled = "native instruction before exit setup state is dead";
    auto code = Program;
    code[7].words[0] = 0x01017816;
    RequireRejected(code, reads, 0x70);

    code = Program;
    code[5].words[0] = 0x00001919;
    RequireRejected(code, reads, 0x70);

    code = Program;
    code[7].words[0] = 0x00010816;
    RequireRejected(code, reads, 0x70);

    code = Program;
    code[8].words[1] = 3;
    RequireRejected(code, reads, 0x80);

    code = Program;
    code[8].words[2] = 0x00703070;
    RequireRejected(code, reads, 0x80);

    code = Program;
    code[7] = Instruction{{0x00017224, 0xff, 0x000e04ff, 0x000fc800}};
    RequireRejected(code, reads, 0x70);
    code[7].words[2] = 0x000e00ff;
    RequireAccepted(code);

    code = Program;
    code[9] = Nop;
    RequireRejected(code, unmodeled, 0x90);

    code = Program;
    code[6] = Instruction{{0x0000194d, 0, 0x03800000, 0x000fea00}};
    RequireRejected(code, unmodeled, 0x60);

    code = Program;
    code[10] = Nop;
    RequireAccepted(code);
}

TEST_CASE("Ampere geometry exit body cannot touch the handler state", "[video_core][ampere]") {
    for (const u32 word0 : {0x00007357U, 0x00007957U, 0x00007355U, 0x0000734eU}) {
        auto code = Program;
        code[10] = Instruction{{word0, 0, 0, 0x000fe200}};
        RequireRejected(code, "native exit handler state access in body", 0xa0);
    }
}

TEST_CASE("Ampere geometry exit body control flow stays inside the body", "[video_core][ampere]") {
    constexpr std::string_view leaves = "native branch leaves exit envelope body";
    constexpr std::string_view control = "native control flow in exit envelope body";
    auto code = Program;
    code[10] = Instruction{{0x00007947, 0x10, 0x03800000, 0x000fea00}};
    RequireAccepted(code);
    code[10].words[1] = 0x20;
    RequireRejected(code, leaves, 0xa0);
    code[10].words[1] = 0xffffff90;
    code[10].words[2] = 0x03800000 | 0x3ffff;
    RequireRejected(code, leaves, 0xa0);

    code = Program;
    code[6].words[1] = 0x60;
    RequireRejected(code, leaves, 0x60);

    code = Program;
    code[10] = Instruction{{0x00007944, 0, 0x03800000, 0x000fea00}};
    RequireRejected(code, control, 0xa0);
    code[10].words[2] |= 0x400000;
    RequireAccepted(code);

    for (const u32 word0 : {0x00007950U, 0x00007949U, 0x0000795bU, 0x0000794aU}) {
        code = Program;
        code[10] = Instruction{{word0, 0, 0x03800000, 0x000fea00}};
        RequireRejected(code, control, 0xa0);
    }
}

TEST_CASE("Ampere geometry exit body EXITs have no extra modes", "[video_core][ampere]") {
    constexpr std::string_view mode = "native body EXIT mode";
    auto code = Program;
    code[11].words[2] = 0x03000000;
    RequireRejected(code, mode, 0xb0);
    code = Program;
    code[11].words[2] |= 0x00100000;
    RequireRejected(code, mode, 0xb0);
    code = Program;
    code[11].words[3] |= 1;
    RequireRejected(code, mode, 0xb0);
    code = Program;
    code[11].words[0] = 0x0000f94d;
    RequireRejected(code, mode, 0xb0);
}
TEST_CASE("Ampere geometry exit lowering branches body exits to the handler",
          "[video_core][ampere]") {
    const auto lowered = Shader::Ampere::LowerToMaxwell(Program, true);
    INFO(lowered.error << " at " << lowered.error_offset);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == 0x100);
    const auto& offsets = lowered.source_instruction_offsets;
    const auto& code = lowered.instructions;
    REQUIRE(offsets.size() == Program.size());
    for (size_t index = 0; index < 5; ++index) REQUIRE(offsets[index + 1] == offsets[index]);
    REQUIRE(offsets[15] == offsets[14]);
    REQUIRE(offsets[15] + 1 == code.size());

    const auto address_of = [](u32 index) { return 0x50U + index * 8 + ((index + 1) / 3) * 8; };
    const auto branch_target = [&](u32 index) {
        const s64 relative = static_cast<s64>((code[index] >> 20) & 0xffffff) << 40 >> 40;
        return s64{address_of(index)} + 8 + relative;
    };
    const u32 handler = offsets[13];
    for (const auto& [native, guard] : {std::pair<size_t, u64>{11, 0}, {12, 7}}) {
        const u32 branch = offsets[native];
        REQUIRE(offsets[native + 1] == branch + 1);
        REQUIRE((code[branch] & ~(u64{0xffffff} << 20)) == (0xe24000000000000fULL | (guard << 16)));
        REQUIRE(branch_target(branch) == address_of(handler));
    }
    REQUIRE(offsets[12] == offsets[11] + 1);

    REQUIRE(offsets[14] == handler + 1);
    const u64 final_output = code[handler];
    REQUIRE((final_output & 0xfff8000000000000ULL) == 0xfbe0000000000000ULL);
    REQUIRE((final_output & 0xff) == 0xff);
    REQUIRE(((final_output >> 8) & 0xff) == 0);
    REQUIRE(((final_output >> 16) & 0xf) == 7);
    REQUIRE(((final_output >> 20) & 0xff) == 0xff);
    REQUIRE(((final_output >> 39) & 3) == 0);

    REQUIRE(code.back() == 0xe30000000007000fULL);
    REQUIRE(std::count_if(code.begin(), code.end(), [](u64 word) {
                return (word & 0xfff0000000000000ULL) == 0xe300000000000000ULL;
            }) == 1);
}
} // namespace
