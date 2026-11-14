// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Warp = std::array<std::array<u32, 768>, 32>;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Execute the emitted Maxwell subset in lockstep, including shuffles and bank copies.
void Execute(Warp& regs, std::span<const u64> code) {
    for (u64 w : code) {
        const Warp before = regs;
        const u32 d = w & 255, a = (w >> 8) & 255, b = (w >> 20) & 255;
        for (u32 lane = 0; lane < 32; ++lane) {
            const auto r = [&](u32 reg) { return reg == 255 ? 0U : before[lane][reg]; };
            if ((w >> 52) == 0x5c9) {
                if (((w >> 44) & 15) == 10)
                    regs[lane][d + a % 4 * 256] = before[lane][b + (a >> 2) * 256];
                else regs[lane][d] = r(b);
            } else if ((w >> 48) == 0xf0c8) {
                regs[lane][d] = lane;
            } else if ((w >> 56) == 0x04) {
                regs[lane][d] = r(a) & static_cast<u32>(w >> 20);
            } else if ((w >> 48) == 0x3848) {
                regs[lane][d] = r(a) << ((w >> 20) & 31);
            } else if ((w >> 56) == 0x1c) {
                regs[lane][d] = r(a) + ((w >> 20) & 0x7ffff);
            } else if ((w >> 52) == 0xef1) {
                regs[lane][d] = b == 255 ? before[0][a] : before[r(b) & 31][a];
            } else if ((w >> 56) == 0x38) {
                u32 value = (r(a) >> ((w >> 20) & 255)) & 255;
                if ((w >> 48) & 1) value = static_cast<u32>(static_cast<s32>(value << 24) >> 24);
                regs[lane][d] = value;
            } else if ((w >> 56) == 0x5b) {
                const auto half = [](u32 value, bool sign) -> s64 {
                    return sign ? static_cast<s16>(value) : static_cast<s64>(value & 65535);
                };
                regs[lane][d] = static_cast<u32>(half(r(a), (w >> 48) & 1) *
                    half(r(b), (w >> 49) & 1) + r((w >> 39) & 255));
            } else if ((w >> 48) != 0xe300) {
                FAIL("Unexpected Maxwell instruction " << std::hex << w);
            }
        }
    }
}
}

TEST_CASE("Ampere IMMA integer fragments produce wrapping matrix products", "[ampere]") {
    for (bool sa : {false, true}) for (bool sb : {false, true}) {
        // D aliases each source in turn; high native GPRs exercise shadow bank copies.
        for (u32 dest : {8U, 12U, 16U, 20U, 244U}) for (bool zero_c : {false, true}) {
            const u32 a = 8, b = 12, c = zero_c ? 255 : 16;
            Instruction native{{0x08007237U | (dest << 16), b,
                0x00400c00U | (u32{sa} << 12) | (u32{sb} << 14) | c, 0}};
            const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{native, Exit});
            INFO(lowered.error);
            REQUIRE(static_cast<bool>(lowered));
            Warp regs{};
            for (u32 lane = 0; lane < 32; ++lane) for (u32 reg = 0; reg < 255; ++reg) {
                const u32 value = (0x9e3779b9U * (lane * 256 + reg + 1)) ^ 0xa5a5a5a5U;
                regs[lane][reg] = regs[lane][256 + reg] = value;
            }
            const Warp before = regs;
            Execute(regs, lowered.instructions);
            const auto byte = [](u32 v, u32 k, bool sign) -> s32 {
                const u32 b = (v >> (8 * (k % 4))) & 255;
                return sign && b >= 128 ? static_cast<s32>(b) - 256 : static_cast<s32>(b);
            };
            for (u32 lane = 0; lane < 32; ++lane) for (u32 d = 0; d < 4; ++d) {
                u32 expected = zero_c ? 0 : before[lane][c + d];
                for (u32 k = 0; k < 32; ++k) {
                    const s32 av = byte(before[4 * (lane / 4) + (k % 16) / 4][a + 2 * (k / 16) + d / 2], k, sa);
                    const s32 bv = byte(before[4 * (2 * (lane % 4) + d % 2) + (k % 16) / 4][b + k / 16], k, sb);
                    expected += static_cast<u32>(av * bv);
                }
                CHECK(regs[lane][(dest >= 244 ? 256 : 0) + dest + d] == expected);
            }
            for (u32 lane = 0; lane < 32; ++lane) for (u32 reg = 0; reg < 244; ++reg)
                if (reg < dest || reg >= dest + 4) CHECK(regs[lane][reg] == before[lane][reg]);
        }
    }
}

TEST_CASE("Ampere IMMA rejects unsupported shapes saturation and divergent predicates", "[ampere]") {
    const Instruction valid{{0x28287237, 0x2e, 0x00405cff, 0}};
    REQUIRE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(std::array{valid, Exit})));
    for (u32 bit : {74U, 75U, 82U, 83U, 84U, 85U, 86U}) {
        auto invalid = valid;
        invalid.words[bit / 32] ^= 1U << (bit % 32);
        CHECK_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(std::array{invalid, Exit})));
    }
    auto guarded = valid;
    guarded.words[0] &= ~0x7000U;
    CHECK_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(std::array{guarded, Exit})));
}
