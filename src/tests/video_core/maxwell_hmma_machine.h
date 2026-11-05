// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

// Test-only executable semantics for the strict subset emitted by HMMA lowering.
// This does NOT multiply matrices: it decodes and executes each emitted word.
namespace HmmaNumeric {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using U16 = std::uint16_t;

inline float HalfToFloat(U16 bits) {
    const U32 sign = U32{bits & 0x8000U} << 16;
    const U32 exponent = (bits >> 10) & 31U;
    U32 fraction = bits & 1023U;
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000U | (fraction << 13));
    if (exponent != 0)
        return std::bit_cast<float>(sign | ((exponent + 112U) << 23) | (fraction << 13));
    if (fraction == 0) return std::bit_cast<float>(sign);
    U32 shift = 0;
    while ((fraction & 1024U) == 0) { fraction <<= 1; ++shift; }
    return std::bit_cast<float>(sign | ((113U - shift) << 23) | ((fraction & 1023U) << 13));
}

inline U32 RoundRightEven(U32 value, U32 shift) {
    if (shift == 0) return value;
    if (shift >= 32) return 0;
    const U32 main = value >> shift;
    const U32 remainder = value & ((U32{1} << shift) - 1);
    const U32 halfway = U32{1} << (shift - 1);
    return main + (remainder > halfway || (remainder == halfway && (main & 1U)));
}

inline U16 FloatToHalf(float value) {
    const U32 bits = std::bit_cast<U32>(value);
    const U32 sign = (bits >> 16) & 0x8000U;
    const U32 exponent = (bits >> 23) & 255U;
    const U32 fraction = bits & 0x7fffffU;
    if (exponent == 255)
        return static_cast<U16>(sign | 0x7c00U | (fraction ? 0x200U | (fraction >> 13) : 0U));
    if (exponent < 102) return static_cast<U16>(sign);
    if (exponent < 113)
        return static_cast<U16>(sign | RoundRightEven(0x800000U | fraction, 126U - exponent));
    if (exponent > 142) return static_cast<U16>(sign | 0x7c00U);
    const U32 significand = RoundRightEven(fraction, 13);
    // Carry from mantissa rounding deliberately advances the exponent (or Infinity).
    return static_cast<U16>(sign | (((exponent - 112U) << 10) + significand));
}

class Machine {
public:
    static constexpr U32 Lanes = 32;
    static constexpr U32 Registers = 3 * 256;
    using Warp = std::array<std::array<U32, Registers>, Lanes>;
    Warp registers{};
    std::array<std::array<bool, 7>, Lanes> predicates{};
    std::size_t executed_words{};
    std::size_t shuffles{};
    std::size_t fmas{};

    U32 Read(U32 lane, U32 reg) const {
        if (lane >= Lanes || reg >= Registers) throw std::runtime_error("Register out of range");
        return reg == 255 ? 0U : registers[lane][reg];
    }

    void Run(std::span<const U64> program) {
        for (const U64 word : program) Step(word);
    }

private:
    static constexpr U64 PredMask = U64{15} << 16;
    static void Encoding(U64 word, U64 base, U64 variables) {
        if ((word & ~variables) != base)
            throw std::runtime_error("Unsupported modifier/encoding: " + std::to_string(word));
    }

    void Step(U64 word) {
        // Simultaneous lane reads matter: writing lane0 before reading it from lane1
        // would silently invalidate the SHFL test. Commit only after all32 lanes.
        std::array<U32, Lanes> output{};
        U32 destination = static_cast<U32>(word & 255U);
        const U32 guard = static_cast<U32>((word >> 16) & 15U);
        const U32 ra = static_cast<U32>((word >> 8) & 255U);
        const U32 rb = static_cast<U32>((word >> 20) & 255U);
        const U32 rc = static_cast<U32>((word >> 39) & 255U);
        const U32 top = static_cast<U32>(word >> 48);
        enum class Code { BankCopy, Mov32, LaneId, And32, Shl, Add, Shuffle, Convert, Fma, ScaledAdd };
        Code operation;
        if (top == 0x5c98 && ((word >> 44) & 15U) == 10 && ((word >> 39) & 15U) == 0) {
            Encoding(word, 0x5c98a00000000000ULL, 255U | (U64{15} << 8) | PredMask | (U64{255} << 20));
            const U32 dbank = (word >> 8) & 3U;
            const U32 sbank = (word >> 10) & 3U;
            if (dbank > 2 || sbank > 2) throw std::runtime_error("Unsupported register bank");
            destination += dbank * 256;
            operation = Code::BankCopy;
        } else if ((word >> 56) == 0x01) {
            Encoding(word, 0x010000000000f000ULL, 255U | PredMask | (U64{0xffffffffU} << 20));
            operation = Code::Mov32;
        } else if (top == 0xf0c8) {
            Encoding(word, 0xf0c8000000000000ULL, 255U | PredMask);
            operation = Code::LaneId;
        } else if ((word >> 56) == 0x04) {
            Encoding(word, 0x0400000000000000ULL, 65535U | PredMask | (U64{0xffffffffU} << 20));
            operation = Code::And32;
        } else if (top == 0x3848) {
            Encoding(word, 0x3848000000000000ULL, 65535U | PredMask | (U64{31} << 20));
            operation = Code::Shl;
        } else if (top == 0x5c10) {
            Encoding(word, 0x5c10000000000000ULL, 65535U | PredMask | (U64{255} << 20));
            operation = Code::Add;
        } else if (top == 0xef17) {
            Encoding(word, 0xef17007c20000000ULL, 65535U | PredMask | (U64{255} << 20));
            // Only SHFL.IDX, register index, immediate mask0x1f, result predicate PT.
            operation = Code::Shuffle;
            ++shuffles;
        } else if (top == 0x5ca8) {
            Encoding(word, 0x5ca8000000000000ULL,
                     255U | (U64{15} << 8) | PredMask | (U64{255} << 20) | (U64{1} << 41));
            const U32 src_size = (word >> 10) & 3U;
            const U32 dst_size = (word >> 8) & 3U;
            if (!((src_size == 1 && dst_size == 2) || (src_size == 2 && dst_size == 1)))
                throw std::runtime_error("Unsupported F2F size");
            if (src_size == 2 && ((word >> 41) & 1U)) throw std::runtime_error("F32 selector");
            operation = Code::Convert;
        } else if (top == 0x5980) {
            Encoding(word, 0x5980000000000000ULL,
                     65535U | PredMask | (U64{255} << 20) | (U64{255} << 39));
            operation = Code::Fma;
            ++fmas;
        } else if (top == 0x5c18) {
            Encoding(word, 0x5c18000000000000ULL,
                     65535U | PredMask | (U64{255} << 20) | (U64{31} << 39));
            operation = Code::ScaledAdd;
        } else {
            throw std::runtime_error("Unknown emitted opcode: " + std::to_string(word));
        }

        for (U32 lane = 0; lane < Lanes; ++lane) {
            switch (operation) {
            case Code::BankCopy: output[lane] = Read(lane, rb + ((word >> 10) & 3U) * 256); break;
            case Code::Mov32: output[lane] = static_cast<U32>(word >> 20); break;
            case Code::LaneId: output[lane] = lane; break;
            case Code::And32: output[lane] = Read(lane, ra) & static_cast<U32>(word >> 20); break;
            case Code::Shl: output[lane] = Read(lane, ra) << ((word >> 20) & 31U); break;
            case Code::Add: output[lane] = Read(lane, ra) + Read(lane, rb); break;
            case Code::Shuffle: {
                const U32 source_lane = Read(lane, rb) & 31U;
                output[lane] = Read(source_lane, ra);
                break;
            }
            case Code::Convert:
                if (((word >> 10) & 3U) == 1) {
                    const U32 half = (word >> 41) & 1U;
                    output[lane] = std::bit_cast<U32>(HalfToFloat(static_cast<U16>(Read(lane, rb) >> (16 * half))));
                } else {
                    output[lane] = FloatToHalf(std::bit_cast<float>(Read(lane, rb)));
                }
                break;
            case Code::Fma:
                output[lane] = std::bit_cast<U32>(std::fma(std::bit_cast<float>(Read(lane, ra)),
                    std::bit_cast<float>(Read(lane, rb)), std::bit_cast<float>(Read(lane, rc))));
                break;
            case Code::ScaledAdd:
                output[lane] = (Read(lane, ra) << ((word >> 39) & 31U)) + Read(lane, rb);
                break;
            }
        }
        for (U32 lane = 0; lane < Lanes; ++lane) {
            const bool positive = (guard & 7U) == 7U || predicates[lane][guard & 7U];
            const bool active = positive != ((guard & 8U) != 0);
            if (active && destination != 255) registers[lane][destination] = output[lane];
        }
        ++executed_words;
    }
};
} // namespace HmmaNumeric
