// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

u32 EvaluateRedux(const Shader::Ampere::LoweredProgram& program,
                  const std::array<u32, 32>& lanes, u32 active) {
    using Shader::Maxwell::Opcode;
    std::array<u32, 256> registers{};
    const auto read = [&](u32 index) { return index == 255 ? 0U : registers[index]; };
    const auto logical = [](u32 a, u32 b, u32 operation) {
        switch (operation) {
        case 0: return a & b;
        case 1: return a | b;
        case 2: return a ^ b;
        default: FAIL("Unexpected logical operation in reduction"); return 0U;
        }
    };
    for (const u64 instruction : program.instructions) {
        const u32 dest = instruction & 0xff;
        const u32 a = read((instruction >> 8) & 0xff);
        const u32 b = read((instruction >> 20) & 0xff);
        switch (Shader::Maxwell::Decode(instruction)) {
        case Opcode::MOV32I:
            registers[dest] = static_cast<u32>(instruction >> 20);
            break;
        case Opcode::VOTE:
            registers[dest] = active;
            break;
        case Opcode::SHFL:
            registers[dest] = lanes[(instruction >> 20) & 31];
            break;
        case Opcode::SHR_imm:
            registers[dest] = a >> ((instruction >> 20) & 31);
            break;
        case Opcode::LOP32I:
            registers[dest] = logical(a, static_cast<u32>(instruction >> 20),
                                      (instruction >> 53) & 3);
            break;
        case Opcode::IADD_reg:
            registers[dest] = a + ((instruction & (1ULL << 48)) ? 0U - b : b);
            break;
        case Opcode::LOP_reg:
            registers[dest] = logical((instruction & (1ULL << 39)) ? ~a : a,
                                      (instruction & (1ULL << 40)) ? ~b : b,
                                      (instruction >> 41) & 3);
            break;
        case Opcode::IMNMX_reg:
            registers[dest] = (instruction & (1ULL << 42)) ? std::max(a, b) : std::min(a, b);
            break;
        case Opcode::EXIT:
            return registers[221]; // UR5 is mapped to R221.
        default:
            FAIL("Unexpected instruction in lowered reduction");
        }
    }
    FAIL("Reduction did not exit");
    return 0;
}

TEST_CASE("Ampere signed and unsigned min/max reductions exclude inactive lanes",
          "[video_core][ampere]") {
    std::array<u32, 32> lanes{};
    for (u32 i = 0; i < lanes.size(); ++i) lanes[i] = i * 0x9e3779b9U;
    lanes[0] = 0x80000000;
    lanes[1] = 0x7fffffff;
    lanes[2] = 0xffffffff;
    for (const u32 mode : {0x10000U, 0x10200U, 0x14000U, 0x14200U}) {
        // REDUX.MIN.S32 UR5, R0 is the instruction rejected by TOTK's compute shader.
        const std::array<Instruction, 2> code{{
            {{0x000573c4, 0, mode, 0x001e2400}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        for (const u32 active : {0xffffffffU, 1U, 2U, 0x80000000U, 0x55555555U}) {
            std::optional<u32> expected;
            for (u32 lane = 0; lane < lanes.size(); ++lane) {
                if (!(active & (1U << lane))) continue;
                if (!expected) { expected = lanes[lane]; continue; }
                const bool less = (mode & 0x200)
                                      ? std::bit_cast<s32>(lanes[lane]) < std::bit_cast<s32>(*expected)
                                      : lanes[lane] < *expected;
                if (less == ((mode & 0x4000) == 0)) expected = lanes[lane];
            }
            REQUIRE(EvaluateRedux(lowered, lanes, active) == *expected);
        }
    }
}

TEST_CASE("Ampere signed global reductions retain their uniform address and predicate",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0xff00098e, 1, 0x0c90e304, 0x000fe200}}, // @P0 RED.E.MIN.S32 [UR4], R1
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const auto reduction = lowered.instructions.front();
    REQUIRE(Shader::Maxwell::Decode(reduction) == Shader::Maxwell::Opcode::RED);
    REQUIRE((reduction & 0xff) == 1); // R1
    REQUIRE(((reduction >> 8) & 0xff) == 220); // UR4
    REQUIRE(((reduction >> 16) & 0xf) == 0); // P0
    REQUIRE(((reduction >> 20) & 7) == 1); // S32
    REQUIRE(((reduction >> 23) & 7) == 1); // MIN
    REQUIRE(((reduction >> 48) & 1) == 1); // 64-bit address
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
} // namespace
