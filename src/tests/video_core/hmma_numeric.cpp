// SPDX-License-Identifier: GPL-3.0-or-later
#include <cfenv>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "maxwell_hmma_machine.h"

namespace {
using namespace HmmaNumeric;
using Shader::Ampere::Instruction;
using MatrixA = std::array<std::array<double, 16>, 16>;
using MatrixB = std::array<std::array<double, 8>, 16>;
using MatrixC = std::array<std::array<double, 8>, 16>;
struct Inputs { MatrixA a{}; MatrixB b{}; MatrixC c{}; };
struct Registers { U32 a{16}; U32 b{32}; U32 c{48}; U32 d{64}; };

// Independent numeric reference: doubles and arithmetic powers of two, not the
// interpreter's bit conversion and not the lowerer's per-lane shuffle equations.
double HalfValue(U16 bits) {
    const double sign = (bits & 0x8000U) ? -1.0 : 1.0;
    const U32 exponent = (bits >> 10) & 31U;
    const U32 fraction = bits & 1023U;
    if (exponent == 31)
        return fraction ? std::numeric_limits<double>::quiet_NaN()
                        : sign * std::numeric_limits<double>::infinity();
    return sign * (exponent ? std::ldexp(1024.0 + fraction, static_cast<int>(exponent) - 25)
                           : std::ldexp(static_cast<double>(fraction), -24));
}

double HalfRound(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    if (std::abs(value) >= 65520.0)
        return std::copysign(std::numeric_limits<double>::infinity(), value);
    int exponent{};
    std::frexp(std::abs(value), &exponent);
    const double quantum = std::ldexp(1.0, std::max(-24, exponent - 11));
    return std::nearbyint(value / quantum) * quantum;
}

std::vector<U64> Emitted(Registers reg, U32 depth = 16, U32 predicate = 7) {
    // Synthetic HMMA.16816.F16 fields follow the production opcode decoder.
    // The program has one operation+EXIT and contains no captured game bytes.
    const std::array<Instruction, 2> input{{
        {{0x23cU | (predicate << 12) | (reg.d << 16) | (reg.a << 24),
          reg.b, reg.c | (depth == 16 ? 0x800U : 0U), 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input, {.tensor = true});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_instruction_offsets.size() == 2);
    REQUIRE(lowered.source_instruction_offsets[0] == 0);
    return {lowered.instructions.begin(),
            lowered.instructions.begin() + lowered.source_instruction_offsets[1]};
}

void PutHalf(Machine& machine, U32 lane, U32 reg, U32 half, double value) {
    REQUIRE(HalfRound(value) == value); // Fixtures do not silently quantize their inputs.
    const U32 bits = FloatToHalf(static_cast<float>(value));
    auto& target = machine.registers[lane][reg];
    target = (target & ~(U32{0xffff} << (half * 16))) | (bits << (half * 16));
}

Machine Encode(const Inputs& data, Registers reg, U32 depth) {
    Machine machine;
    for (U32 lane = 0; lane < 32; ++lane)
        for (U32 index = 0; index < Machine::Registers; ++index)
            machine.registers[lane][index] = 0xa0000000U ^ (lane * 0x10101U) ^ index;
    // PTX m16n8k16 matrix element -> register fragment mapping. This starts from
    // matrix coordinates, not from the emulator's per-output shuffle traversal.
    for (U32 row = 0; row < 16; ++row)
        for (U32 k = 0; k < depth; ++k)
            PutHalf(machine, (row % 8) * 4 + (k % 8) / 2,
                    reg.a + row / 8 + (k / 8) * 2, k % 2, data.a[row][k]);
    for (U32 k = 0; k < depth; ++k)
        for (U32 col = 0; col < 8; ++col)
            PutHalf(machine, col * 4 + (k % 8) / 2, reg.b + k / 8, k % 2, data.b[k][col]);
    if (reg.c != 255)
        for (U32 row = 0; row < 16; ++row)
            for (U32 col = 0; col < 8; ++col)
                PutHalf(machine, (row % 8) * 4 + col / 2, reg.c + row / 8, col % 2, data.c[row][col]);
    return machine;
}

// Decode actual initial fragments to account for legal source/source aliases too.
Inputs Decode(const Machine& machine, Registers reg, U32 depth) {
    const auto half = [&](U32 lane, U32 index, U32 which) {
        return HalfValue(static_cast<U16>(machine.Read(lane, index) >> (which * 16)));
    };
    Inputs data;
    for (U32 row = 0; row < 16; ++row)
        for (U32 k = 0; k < depth; ++k)
            data.a[row][k] = half((row % 8) * 4 + (k % 8) / 2,
                                  reg.a + row / 8 + (k / 8) * 2, k % 2);
    for (U32 k = 0; k < depth; ++k)
        for (U32 col = 0; col < 8; ++col)
            data.b[k][col] = half(col * 4 + (k % 8) / 2, reg.b + k / 8, k % 2);
    if (reg.c != 255)
        for (U32 row = 0; row < 16; ++row)
            for (U32 col = 0; col < 8; ++col)
                data.c[row][col] = half((row % 8) * 4 + col / 2, reg.c + row / 8, col % 2);
    return data;
}

MatrixC Reference(const Inputs& input, U32 depth, bool round_after_eight) {
    MatrixC expected = input.c;
    for (U32 row = 0; row < 16; ++row) {
        for (U32 col = 0; col < 8; ++col) {
            for (U32 k = 0; k < depth; ++k) {
                expected[row][col] += input.a[row][k] * input.b[k][col];
                if (round_after_eight && k % 8 == 7)
                    expected[row][col] = HalfRound(expected[row][col]);
            }
            expected[row][col] = HalfRound(expected[row][col]);
        }
    }
    return expected;
}

MatrixC Check(Inputs input, Registers reg = {}, U32 depth = 16, bool round_after_eight = false,
              U32 guard = 7, bool enabled = true) {
    auto machine = Encode(input, reg, depth);
    for (auto& lane : machine.predicates) lane.fill(enabled);
    const auto before = machine.registers;
    const auto expected = Reference(Decode(machine, reg, depth), depth, round_after_eight);
    const auto words = Emitted(reg, depth, guard);
    machine.Run(words);
    REQUIRE(machine.fmas == 4 * depth);
    REQUIRE(machine.shuffles == 2 * depth);
    MatrixC actual{};
    for (U32 row = 0; row < 16; ++row) {
        for (U32 col = 0; col < 8; ++col) {
            const U32 lane = (row % 8) * 4 + col / 2;
            const U32 index = reg.d + row / 8;
            const U32 which = col % 2;
            actual[row][col] = HalfValue(static_cast<U16>(machine.Read(lane, index) >> (which * 16)));
            INFO("row=" << row << " col=" << col << " lane=" << lane << " dst=" << reg.d);
            if (enabled) CHECK(actual[row][col] == expected[row][col]);
            else CHECK(machine.Read(lane, index) == before[lane][index]);
        }
    }
    // Includes the four temporarily borrowed registers. Scratch246..254 and
    // shadow bank2 are implementation-owned and intentionally excluded.
    for (U32 lane = 0; lane < 32; ++lane)
        for (U32 index = 0; index < 246; ++index)
            if (index != reg.d && index != reg.d + 1)
                CHECK(machine.Read(lane, index) == before[lane][index]);
    return actual;
}

Inputs Pattern() {
    Inputs input;
    for (U32 row = 0; row < 16; ++row)
        for (U32 k = 0; k < 16; ++k)
            input.a[row][k] = static_cast<int>((row * 3 + k * 5) % 7) - 3;
    for (U32 k = 0; k < 16; ++k)
        for (U32 col = 0; col < 8; ++col)
            input.b[k][col] = static_cast<int>((k * 7 + col * 3) % 5) - 2;
    for (U32 row = 0; row < 16; ++row)
        for (U32 col = 0; col < 8; ++col)
            input.c[row][col] = static_cast<int>((row + 2 * col) % 13) - 6;
    return input;
}

TEST_CASE("HMMA interpreter half conversion matches all finite encodings", "[hmma_numeric]") {
    REQUIRE(std::fegetround() == FE_TONEAREST);
    for (U32 bits = 0; bits < 65536; ++bits) {
        if ((bits & 0x7c00U) == 0x7c00U) continue;
        const auto half = static_cast<U16>(bits);
        REQUIRE(static_cast<double>(HalfToFloat(half)) == HalfValue(half));
        REQUIRE(FloatToHalf(HalfToFloat(half)) == half);
    }
    CHECK(FloatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3c00U);
    CHECK(FloatToHalf(1.0f + 3 * std::ldexp(1.0f, -11)) == 0x3c02U);
    CHECK(FloatToHalf(std::ldexp(1.0f, -25)) == 0U);
    CHECK(FloatToHalf(3 * std::ldexp(1.0f, -25)) == 2U);
    CHECK(FloatToHalf(65519.0f) == 0x7bffU);
    CHECK(FloatToHalf(65520.0f) == 0x7c00U);
}

TEST_CASE("Real HMMA emitted words reproduce integer dense matrices and C", "[hmma_numeric]") {
    REQUIRE(std::getenv("EDEN_NX2_TENSOR") == nullptr);
    Check(Pattern());
    Check(Pattern(), {.a=16, .b=32, .c=255, .d=64});
    Check(Pattern(), {}, 8);
    Check(Pattern(), {}, 16, false, 0, true);
    Check(Pattern(), {}, 16, false, 0, false);
}

TEST_CASE("HMMA fragment mapping covers every k and both output row halves", "[hmma_numeric]") {
    for (U32 selected = 0; selected < 16; ++selected) {
        Inputs input;
        for (U32 row = 0; row < 16; ++row)
            for (U32 k = 0; k < 16; ++k) input.a[row][k] = row * 32 + k;
        for (U32 col = 0; col < 8; ++col) input.b[selected][col] = col % 2 ? -1.0 : 1.0;
        const auto result = Check(input, {.a=16, .b=32, .c=255, .d=64});
        for (U32 row = 0; row < 16; ++row)
            for (U32 col = 0; col < 8; ++col)
                CHECK(result[row][col] == (row * 32 + selected) * (col % 2 ? -1.0 : 1.0));
    }
    Inputs identity;
    for (U32 i = 0; i < 16; ++i) identity.a[i][i] = 1.0;
    for (U32 k = 0; k < 16; ++k)
        for (U32 col = 0; col < 8; ++col) identity.b[k][col] = k * 8 + col;
    Check(identity);
}

TEST_CASE("HMMA emitted code preserves source aliases and borrowed registers", "[hmma_numeric]") {
    for (const U32 destination : {16U, 18U, 32U, 48U, 240U})
        Check(Pattern(), {.a=16, .b=32, .c=48, .d=destination});
    Check(Pattern(), {.a=16, .b=32, .c=16, .d=64});
    Check(Pattern(), {.a=16, .b=16, .c=48, .d=64});
}

TEST_CASE("HMMA cancellation follows the explicit eight-product rounding model", "[hmma_numeric]") {
    // Exact dyadic arithmetic before rounding, independently quantized with
    // powers-of-two. This verifies the IMPLEMENTED model, not unspecified
    // console tensor-core accumulation precision/order for general FP16 inputs.
    Inputs input;
    for (auto& row : input.a) row.fill(1.0);
    for (U32 col = 0; col < 8; ++col) {
        input.b[0][col] = 2048.0;
        input.b[1][col] = 1.0;
        input.b[8][col] = -2048.0;
    }
    const auto output = Check(input, {}, 16, true);
    CHECK(output[0][0] == 0.0);
    CHECK(Reference(input, 16, false)[0][0] == 1.0); // Detect accidental final-only rounding.

    Inputs small;
    for (U32 row = 0; row < 16; ++row) small.a[row][0] = std::ldexp(1.0, -14);
    for (U32 col = 0; col < 8; ++col) small.b[0][col] = std::ldexp(1.0, -10);
    const auto tiny = Check(small, {}, 16, true);
    CHECK(tiny[0][0] == std::ldexp(1.0, -24));
}

TEST_CASE("HMMA interpreter fails closed on unknown or modified instructions", "[hmma_numeric]") {
    Machine machine;
    const std::array unknown{U64{0xffffffffffffffffULL}};
    CHECK_THROWS_AS(machine.Run(unknown), std::runtime_error);
    auto words = Emitted({});
    bool altered = false;
    for (auto& word : words) {
        if ((word >> 48) == 0x5980) { word |= U64{1} << 48; altered = true; break; }
    }
    REQUIRE(altered);
    CHECK_THROWS_AS(machine.Run(words), std::runtime_error);
}
} // namespace
