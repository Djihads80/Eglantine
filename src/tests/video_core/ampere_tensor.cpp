// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
namespace {
using Wave = std::array<std::array<u32, 256>, 32>;
u16 encode(float value) {
    // These exact-integer test vectors avoid unspecified tensor accumulation rounding.
    REQUIRE(std::isfinite(value));
    REQUIRE(value == std::trunc(value));
    REQUIRE(std::abs(value) <= 2048);
    if (value == 0)
        return 0;
    const u32 bits = std::bit_cast<u32>(value);
    return static_cast<u16>(((bits >> 16) & 0x8000) | ((((bits >> 23) & 255) - 112) << 10) |
                            ((bits >> 13) & 1023));
}
u32 pack(float a, float b) {
    return encode(a) | (u32{encode(b)} << 16);
}
float half(u32 packed, u32 index) {
    const u32 bits = (packed >> (16 * index)) & 0xffff;
    if ((bits & 0x7fff) == 0)
        return 0;
    return std::ldexp(float(1024 + (bits & 1023)), int((bits >> 10) & 31) - 25) *
           ((bits & 0x8000) ? -1 : 1);
}
void set_mode(const char* value) {
#ifdef _WIN32
    _putenv_s("EDEN_NX2_TENSOR", value ? value : "");
#else
    if (value)
        setenv("EDEN_NX2_TENSOR", value, 1);
    else
        unsetenv("EDEN_NX2_TENSOR");
#endif
}
struct RestoreMode {
    std::optional<std::string> previous;
    RestoreMode() {
        if (const char* value = std::getenv("EDEN_NX2_TENSOR"))
            previous = value;
    }
    ~RestoreMode() {
        set_mode(previous ? previous->c_str() : nullptr);
    }
};
void execute(Wave& r, const std::vector<u64>& code) {
    for (auto w : code) {
        if ((w >> 48) == 0xe300)
            break;
        auto old = r;
        for (u32 t = 0; t < 32; ++t) {
            auto dst = w & 255, a = (w >> 8) & 255, b = (w >> 20) & 255, c = (w >> 39) & 255;
            u32 x = old[t][a], y = old[t][b], imm = (u32)(w >> 20), out = 0;
            const auto op = (w >> 48) & 0xfff8;
            if (op == 0xf0c8) {
                REQUIRE(((w >> 20) & 255) == 0);
                out = t;
            } else if (op == 0x5c98)
                out = y;
            else if ((w >> 56) == 1)
                out = imm;
            else if ((w >> 58) == 1) {
                REQUIRE(((w >> 53) & 3) == 0);
                out = x & imm;
            } else if ((w >> 57) == 14)
                out = x + imm;
            else if (op == 0x3848)
                out = x << ((w >> 20) & 31);
            else if (op == 0x3828)
                out = x >> ((w >> 20) & 31);
            else if (op == 0x5c10)
                out = x + y;
            else if (op == 0x5c18)
                out = (x << ((w >> 39) & 31)) + y;
            else if (op == 0xef10)
                out = old[old[t][b] & 31][a];
            else if (op == 0x5d00) {
                REQUIRE(((w >> 28) & 3) == 0);
                REQUIRE(((w >> 35) & 3) == 0);
                REQUIRE(((w >> 49) & 3) == 0);
                auto sw = (w >> 47) & 3;
                REQUIRE((sw == 2 || sw == 3));
                float aa = half(x, sw - 2);
                out = pack(std::fma(aa, half(y, 0), half(old[t][c], 0)),
                           std::fma(aa, half(y, 1), half(old[t][c], 1)));
            } else {
                FAIL("Unsupported emitted word " << std::hex << w);
            }
            if (dst != 255)
                r[t][dst] = out;
        }
    }
}
} // namespace

TEST_CASE("Software HMMA executes captured fragments as matrix products", "[ampere-tensor]") {
    RestoreMode restore;
    set_mode("emulate");
    for (const u32 destination : {48U, 8U, 16U}) {
        const u32 accumulator = destination == 48 ? 255 : 16;
        const std::array code{Shader::Ampere::Instruction{{0x0800723c | (destination << 16), 12,
                                                           0x800 | accumulator, 0x041ff000}},
                              Shader::Ampere::Instruction{{0x0000794d, 0, 0x03800000, 0}}};
        auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(lowered);
        for (int seed = 0; seed < 32; ++seed) {
            float A[16][16], B[16][8];
            for (int i = 0; i < 16; ++i) {
                for (int k = 0; k < 16; ++k)
                    A[i][k] = (i * 13 + k * 7 + seed) % 3 - 1;
                for (int j = 0; j < 8; ++j)
                    B[i][j] = (i * 3 + j * 5 + seed) % 5 - 2;
            }
            Wave r{};
            for (int t = 0; t < 32; ++t) {
                int g = t / 4, i = t % 4;
                for (int k = 0; k < 2; ++k) {
                    r[t][8 + k * 2] = pack(A[g][k * 8 + i * 2], A[g][k * 8 + i * 2 + 1]);
                    r[t][9 + k * 2] = pack(A[g + 8][k * 8 + i * 2], A[g + 8][k * 8 + i * 2 + 1]);
                    r[t][12 + k] = pack(B[k * 8 + i * 2][g], B[k * 8 + i * 2 + 1][g]);
                }
            }
            if (accumulator != 255) {
                for (int t = 0; t < 32; ++t)
                    for (int row = 0; row < 2; ++row) {
                        const int i = t / 4 + row * 8, j = 2 * (t % 4);
                        r[t][accumulator + row] =
                            pack((i + j + seed) % 5 - 2, (i + j + 1 + seed) % 5 - 2);
                    }
            }
            execute(r, lowered.instructions);
            for (int t = 0; t < 32; ++t)
                for (int row = 0; row < 2; ++row)
                    for (int col = 0; col < 2; ++col) {
                        int i = t / 4 + 8 * row, j = 2 * (t % 4) + col;
                        float expected = accumulator == 255 ? 0 : (i + j + seed) % 5 - 2;
                        for (int k = 0; k < 16; ++k)
                            expected += A[i][k] * B[k][j];
                        float got = half(r[t][destination + row], col);
                        CAPTURE(seed, i, j);
                        REQUIRE(got == expected);
                    }
        }
    }
}

TEST_CASE("Software HMMA refuses stub and unknown modes", "[ampere-tensor]") {
    RestoreMode restore;
    const std::array code{Shader::Ampere::Instruction{{0x0830723c, 12, 0x000008ff, 0x041ff000}},
                          Shader::Ampere::Instruction{{0x0000794d, 0, 0x03800000, 0}}};
    for (const char* mode : {"stub", "unknown"}) {
        set_mode(mode);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
    }
}
