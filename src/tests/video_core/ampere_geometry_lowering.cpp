// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x001fea00}};
constexpr Instruction Final{{0x00ff7324, 0xff, 0, 0x0001e200}};
}

TEST_CASE("Ampere geometry instructions require an explicit geometry stage",
          "[video_core][ampere][geometry]") {
    for (const Instruction instruction : std::array{
             Final,
             Instruction{{0x030c7923, 0, 0, 0x000e2800}},
             Instruction{{0x00027919, 0, 0x1100, 0x000fe400}},
             Instruction{{0x00027919, 0, 0x1d00, 0x000fe400}},
             Instruction{{0x0000734e, 0, 0, 0x000fe400}}}) {
        REQUIRE_FALSE(LowerToMaxwell(std::array{instruction, Exit}));
    }
}

TEST_CASE("Ampere geometry finalization preserves its handle and does not emit or cut",
          "[video_core][ampere][geometry]") {
    const auto lowered = LowerToMaxwell(std::array{Final, Exit}, true);
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    const u64 out = lowered.instructions.front();
    REQUIRE((out & 0xff) == 255);
    REQUIRE(((out >> 8) & 0xff) == 0);
    REQUIRE(((out >> 39) & 3) == 0);
    REQUIRE(lowered.instructions.back() == 0xe30000000007000fULL);
}

TEST_CASE("Ampere geometry emits and cuts with the original predicate and stream",
          "[video_core][ampere][geometry]") {
    for (const u32 opcode : {0x324U, 0x924U}) {
        for (u32 type = 1; type <= 3; ++type) {
            for (u32 predicate = 0; predicate < 15; ++predicate) {
                Instruction out{{0x03ff0000 | (predicate << 12) | opcode, 2, type << 14, 0}};
                const auto lowered = LowerToMaxwell(std::array{out, Exit}, true);
                INFO(lowered.error);
                REQUIRE(lowered);
                REQUIRE(lowered.instructions.size() == 2);
                const u64 word = lowered.instructions.front();
                REQUIRE((word & 0xff) == 255);
                REQUIRE(((word >> 8) & 0xff) == 3);
                REQUIRE(((word >> 16) & 15) == predicate);
                REQUIRE(((word >> 20) & 0x7ffff) == 2);
                REQUIRE(((word >> 39) & 3) == type);
            }
        }
    }
}

TEST_CASE("Ampere geometry rejects unrepresented finalization and stream modes",
          "[video_core][ampere][geometry]") {
    for (const Instruction out : std::array{
             Instruction{{0x00ff7924, 0, 0, 0}},
             Instruction{{0x00007324, 255, 0, 0}},
             Instruction{{0x00ff7324, 0, 0, 0}},
             Instruction{{0x00ff7324, 255, 1, 0}},
             Instruction{{0x00ff7324, 0x100, 0x4000, 0}},
             Instruction{{0x00ff7924, 0x80000, 0x4000, 0}}}) {
        REQUIRE_FALSE(LowerToMaxwell(std::array{out, Exit}, true));
    }
}

TEST_CASE("Ampere geometry stage buffer selectors preserve vertex ordinals",
          "[video_core][ampere][geometry]") {
    const Instruction read{{0x030c7923, 0, 0, 0x000e2800}};
    const auto lowered = LowerToMaxwell(std::array{read, Exit}, true);
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.front() == 0x5c9807800037000cULL);
    for (const Instruction invalid : std::array{
             Instruction{{0xff0c7923, 0, 0, 0}},
             Instruction{{0x030c7923, 1, 0, 0}},
             Instruction{{0x030c7923, 0, 1, 0}}}) {
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}, true));
    }
}

TEST_CASE("Ampere geometry invocation registers use the existing host geometry ABI",
          "[video_core][ampere][geometry]") {
    for (const u32 special : {17U, 29U}) {
        const Instruction read{{0x00027919, 0, special << 8, 0x000fe400}};
        const auto lowered = LowerToMaxwell(std::array{read, Exit}, true);
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.front() == (0xf0c8000000070002ULL | (u64{special} << 20)));
    }
}

TEST_CASE("Ampere geometry index conversion retains U16 truncation and FTZ",
          "[video_core][ampere][geometry]") {
    const Instruction convert{{0x00027305, 2, 0x0021e800, 0x002fe200}};
    const auto lowered = LowerToMaxwell(std::array{convert, Exit}, true);
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 4);
    const u64 word = lowered.instructions[2];
    REQUIRE((word & 0xff) == 2);
    REQUIRE(((word >> 8) & 3) == 1);
    REQUIRE(((word >> 10) & 3) == 2);
    REQUIRE(((word >> 12) & 1) == 0);
    REQUIRE(((word >> 39) & 3) == 3);
    REQUIRE(((word >> 44) & 1) == 1);
    for (const u32 bit : {0U, 8U, 13U, 18U, 22U, 27U}) {
        auto invalid = convert;
        invalid.words[2] ^= 1U << bit;
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}, true));
    }
}
