// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr std::array<Instruction, 2> RegisterTruncation{{
    {{0x00027305, 3, 0x0020f000, 0x000e3000}},
    {{0x0000794d, 0, 0x03800000, 0}},
}};

std::vector<u64> ExpectedTruncation(u32 predicate, u32 source, bool is_signed, bool ftz) {
    const u64 guard = u64{predicate} << 16;
    std::vector<u64> expected;
    if (!is_signed) {
        expected = {
            0x5800000000000000ULL | guard | 250 | (u64{source} << 8) | (u64{source} << 20) |
                (7ULL << 39) | (7ULL << 48),
            0x5c40000000000000ULL | guard | 250 | (u64{source} << 8) | (250ULL << 20) |
                (7ULL << 48),
            0x0100000000000000ULL | guard | 252 | (15ULL << 12) | (0x4f800000ULL << 20),
            0x5800000000000000ULL | guard | 251 | (250ULL << 8) | (252ULL << 20) |
                (7ULL << 39) | (6ULL << 48),
            0x0100000000000000ULL | guard | 252 | (15ULL << 12) | (0x4f7fffffULL << 20),
            0x5c60000000000000ULL | guard | 250 | (250ULL << 8) | (252ULL << 20) |
                (7ULL << 39),
        };
        source = 250;
    }
    expected.push_back(0x5cb0000000000000ULL | guard | 2 | (2ULL << 8) | (2ULL << 10) |
                       (u64{is_signed} << 12) | (u64{source} << 20) | (3ULL << 39) |
                       (u64{ftz} << 44));
    if (!is_signed) {
        expected.push_back(0x5c40000000000000ULL | guard | 2 | (2ULL << 8) | (251ULL << 20) |
                           (1ULL << 41) | (7ULL << 48));
    }
    return expected;
}

void RequireLowered(const std::array<Instruction, 2>& code, const std::vector<u64>& expected) {
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == expected.size() + 1);
    REQUIRE(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));
}

TEST_CASE("Ampere 32-bit truncating F2I keeps FTZ, signedness and predicates",
          "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const bool ftz : {false, true}) {
            for (const u32 predicate : {2U, 7U, 10U}) {
                auto code = RegisterTruncation;
                code[0].words[0] = 0x00020305U | (predicate << 12);
                code[0].words[2] = 0x0020f000U | (ftz ? 0x10000U : 0U) | (is_signed ? 0x100U : 0U);
                RequireLowered(code, ExpectedTruncation(predicate, 3, is_signed, ftz));
            }
        }
    }
}

TEST_CASE("Ampere 32-bit truncating F2I materializes constant and uniform sources",
          "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const bool ftz : {false, true}) {
            const u32 mode = 0x0020f000U | (ftz ? 0x10000U : 0U) | (is_signed ? 0x100U : 0U);
            auto constant = RegisterTruncation;
            constant[0].words = {0x00027b05, 0x00400400, mode, 0x000e3000};
            auto expected = ExpectedTruncation(7, 252, is_signed, ftz);
            expected.insert(expected.begin(), 0x4c98000000070000ULL | 252 | (15ULL << 39) |
                                                  (4ULL << 20) | (1ULL << 34));
            RequireLowered(constant, expected);
            for (const u32 source : {0U, 5U, 22U, 27U}) {
                auto uniform = RegisterTruncation;
                uniform[0].words = {0x00027d05, source, mode | 0x08000000U, 0x000e3000};
                const u32 mapped = source < 22 ? 216 + source : 218 + source;
                RequireLowered(uniform, ExpectedTruncation(7, mapped, is_signed, ftz));
            }
        }
    }
}

TEST_CASE("Ampere unflushed truncating F2I rejects reserved fields and modes",
          "[video_core][ampere]") {
    for (const u32 mode : {0x0020f800U, 0x0020f400U, 0x0020f200U, 0x0020d000U, 0x0060f000U,
                           0x0820f000U}) {
        auto invalid = RegisterTruncation;
        invalid[0].words[2] = mode;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 bit : {0U, 8U, 30U, 31U}) {
        auto invalid = RegisterTruncation;
        invalid[0].words[3] ^= 1U << bit;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (u32 bit = 9; bit < 30; ++bit) {
        auto scheduling = RegisterTruncation;
        scheduling[0].words[3] ^= 1U << bit;
        REQUIRE(static_cast<bool>(LowerToMaxwell(scheduling)));
    }
    auto invalid = RegisterTruncation;
    invalid[0].words[0] |= 1U << 24;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = RegisterTruncation;
    invalid[0].words[1] = 0x40000003;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = RegisterTruncation;
    invalid[0].words = {0x00027b05, 0x40400400, 0x0020f000, 0x000e3000};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = RegisterTruncation;
    invalid[0].words = {0x00027d05, 5, 0x0020f000, 0x000e3000};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    for (const u32 source : {28U, 62U, 64U}) {
        invalid = RegisterTruncation;
        invalid[0].words = {0x00027d05, source, 0x0820f000, 0x000e3000};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
}

} // namespace
