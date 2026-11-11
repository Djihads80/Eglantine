// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

u64 Find(const Shader::Ampere::LoweredProgram& program, u64 opcode, u32 shift) {
    const auto it = std::find_if(program.instructions.begin(), program.instructions.end(),
                                 [&](u64 word) { return word >> shift == opcode; });
    REQUIRE(it != program.instructions.end());
    return *it;
}

TEST_CASE("Ampere formatted surfaces preserve dimensions and descriptor sources",
          "[video_core][ampere]") {
    for (const u32 dimension : {0U, 1U, 2U, 3U, 4U, 5U}) {
        for (const bool bindless : {false, true}) {
            CAPTURE(dimension, bindless);
            std::array code{Instruction{{bindless ? 0x08047998U : 0x08047b97U,
                                         (dimension << 29) | (bindless ? 0U : 0x00c00400U),
                                         0x001e0100U | (bindless ? 12U : 0U), 0}},
                            EXIT};
            const auto load = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(load));
            const u64 word = Find(load, 0xeb0, 52);
            CHECK((word & 0xff) == 4);
            CHECK(((word >> 8) & 0xff) == 8);
            CHECK(((word >> 20) & 0xf) == 1);
            CHECK(((word >> 33) & 7) == dimension);
            CHECK(((word >> 51) & 1) == !bindless);
            CHECK(load.required_texture_buffer == (bindless ? ~0U : 3U));
            if (bindless)
                CHECK(((word >> 39) & 0xff) == 12);
            else
                CHECK(((word >> 36) & 0x1fff) == 4);

            code[0] = {{bindless ? 0x0800799cU : 0x08007b9bU,
                        (dimension << 29) | (bindless ? 4U : 0x00c00404U),
                        0x00100f00U | (bindless ? 12U : 0U), 0}};
            const auto store = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(store));
            const u64 stored = Find(store, 0xeb2, 52);
            CHECK((stored & 0xff) == 4);
            CHECK(((stored >> 33) & 7) == dimension);
            CHECK(((stored >> 20) & 0xf) == 15);
            CHECK(((stored >> 51) & 1) == !bindless);
        }
    }
}

TEST_CASE("Ampere integer surface atomics preserve operation and signedness",
          "[video_core][ampere]") {
    for (const bool bindless : {false, true}) {
        for (const bool returns_value : {false, true}) {
            for (u32 operation = 0; operation <= (returns_value ? 8U : 7U); ++operation) {
                for (const u32 type : {0U, 1U}) {
                    CAPTURE(bindless, returns_value, operation, type);
                    const u32 opcode = bindless ? (returns_value ? 0x394U : 0x3a0U)
                                                : (returns_value ? 0x593U : 0x59fU);
                    const std::array code{
                        Instruction{{0x08047000U | opcode,
                                     0x20000006U | (bindless ? 0U : 0x00c00400U),
                                     (returns_value ? 0x001ee000U : 0x0010e000U) |
                                         (operation << 23) | (type << 9) | (bindless ? 12U : 0U),
                                     0}},
                        EXIT};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(static_cast<bool>(lowered));
                    const u64 word =
                        Find(lowered, returns_value ? 0xeaU : 0xeb4U, returns_value ? 56 : 52);
                    CHECK(((word >> 33) & 7) == 1); // 1D buffer
                    CHECK(((word >> (returns_value ? 29 : 24)) & (returns_value ? 15 : 7)) ==
                          operation);
                    CHECK(((word >> (returns_value ? 51 : 20)) & 7) == type);
                    CHECK(((word >> 8) & 0xff) == 8);
                    CHECK(((word >> (returns_value ? 20 : 0)) & 0xff) == 6);
                    if (returns_value)
                        CHECK((word & 0xff) == 4);
                    CHECK(((word >> (returns_value ? 54 : 51)) & 1) ==
                          (returns_value ? bindless : !bindless));
                }
            }
        }
    }
    const std::array unsupported{Instruction{{0x08047593, 0x20c00406, 0x001ee400, 0}}, EXIT}; // U64
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(unsupported)));
    const std::array reserved_mode{
        Instruction{{0x0800759f, 0x20c00406, 0x0410e000, 0}}, EXIT};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(reserved_mode)));
}

TEST_CASE("Ampere BRA.U keeps the ordinary branch predicate and target", "[video_core][ampere]") {
    std::array code{Instruction{{0x00008947, 0x11, 0x03800000, 0}}, EXIT, EXIT};
    const auto uniform = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(uniform));
    code[0].words[1] &= ~3U;
    const auto ordinary = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(ordinary));
    CHECK(uniform.instructions == ordinary.instructions);
}

TEST_CASE("Ampere large IMNMX immediates are materialized without truncation",
          "[video_core][ampere]") {
    for (const u32 value : {0x80000U, 0x1fffffffU, 0x80000000U, 0xfff7ffffU}) {
        const std::array code{Instruction{{0x02037817, value, 0x03800000, 0}}, EXIT};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u64 move = Find(lowered, 0x010, 52);
        CHECK(static_cast<u32>(move >> 20) == value);
        const u64 minmax = Find(lowered, 0x5c2, 52);
        CHECK(((minmax >> 20) & 255) == (move & 255));
        CHECK(((minmax >> 8) & 255) == 2);
    }
}

TEST_CASE("Ampere constant and uniform shifts preserve the shifted value and amount",
          "[video_core][ampere]") {
    const std::array constant{Instruction{{0xff007619, 0x01000000, 0x00010604, 0}}, EXIT};
    const auto cbuf_shift = LowerToMaxwell(constant);
    REQUIRE(static_cast<bool>(cbuf_shift));
    const u64 load = Find(cbuf_shift, 0x4c98, 48);
    CHECK(((load >> 34) & 31) == 4);
    const u64 shift = Find(cbuf_shift, 0x5c48, 48);
    CHECK(((shift >> 8) & 255) == (load & 255));
    CHECK(((shift >> 20) & 255) == 4);

    const std::array uniform{Instruction{{0xff327c19, 5, 0x08010637, 0}}, EXIT};
    const auto uniform_shift = LowerToMaxwell(uniform);
    REQUIRE(static_cast<bool>(uniform_shift));
    const u64 shl = Find(uniform_shift, 0x5c48, 48);
    CHECK(((shl >> 8) & 255) == 55);
    CHECK(((shl >> 20) & 255) == 221); // UR5 mirror

    const std::array signed_shift{Instruction{{0x3f057299, 4, 0x08011405, 0}}, EXIT};
    const auto arithmetic = LowerToMaxwell(signed_shift);
    REQUIRE(static_cast<bool>(arithmetic));
    const u64 shr = Find(arithmetic, 0x5c2, 52);
    CHECK(((shr >> 48) & 1) == 1);
    CHECK(((shr >> 8) & 255) == 221);
    CHECK(((shr >> 20) & 255) == 220);
}

TEST_CASE("Ampere uniform queries stage the handle before the mip level", "[video_core][ampere]") {
    // The first result is discarded; the second pair starts at R0.
    const std::array code{Instruction{{0x00ff7f6f, 0x00000400, 0x08000f00, 0}}, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.required_texture_buffer == ~0U);
    const u64 txq = Find(lowered, 0xdf5, 52);
    CHECK(((txq >> 8) & 255) == 252);
    CHECK(((txq >> 31) & 15) == 15);
    CHECK(((txq >> 22) & 7) == 1);
}

TEST_CASE("Ampere PIXLD sample index uses Maxwell MY_INDEX", "[video_core][ampere]") {
    const std::array code{Instruction{{0x00047925, 0, 0x000ec000, 0}}, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 word = Find(lowered, 0xefe8, 48);
    CHECK((word & 255) == 4);
    CHECK(((word >> 8) & 255) == 255);
    CHECK(((word >> 31) & 7) == 5);
    CHECK(((word >> 45) & 7) == 7);
}

TEST_CASE("Ampere multisample fetch keeps the sample index separate from the handle",
          "[video_core][ampere]") {
    for (const bool uniform : {false, true}) {
        const std::array code{
            Instruction{{uniform ? 0x08047f66U : 0x08047b66U, uniform ? 0x3000040cU : 0x30c0040cU,
                         uniform ? 0x089e4100U | 255U : 0x009e41ffU, 0}},
            EXIT};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const u64 fetch = Find(lowered, uniform ? 0xddU : 0xdcU, 56);
        CHECK(((fetch >> 50) & 1) == 1);
        CHECK(((fetch >> 55) & 1) == 0); // LZ
        CHECK(((fetch >> 28) & 7) == 2); // 2D
        CHECK(((fetch >> 31) & 15) == 1);
        CHECK(((fetch >> 20) & 255) == 246);
        const u32 sample_target = uniform ? 247 : 246;
        CHECK(std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
            return (word >> 48) == 0x5c98 && (word & 255) == sample_target &&
                   ((word >> 20) & 255) == 12;
        }));
    }
}

TEST_CASE("Ampere TXD.B stages the handle before its coordinate list", "[video_core][ampere]") {
    const std::array code{Instruction{{0x0804736d, 0x28000010, 0x001e0f0c, 0}}, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 txd = Find(lowered, 0xde, 56);
    CHECK(((txd >> 54) & 1) == 1); // bindless
    CHECK(((txd >> 8) & 255) == 248);
    CHECK(((txd >> 20) & 255) == 16);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
            return (word >> 48) == 0x5c98 && (word & 255) == 248 + i &&
                   ((word >> 20) & 255) == 8 + i;
        }));
    }
}

TEST_CASE("Ampere global address shadowing distinguishes Ra.U32 from Ra.64",
          "[video_core][ampere]") {
    Instruction load{{0xdbdb7981, 26, 0x081e890a, 0}};
    const auto narrow = Shader::Ampere::PrepareRegisterPressure(load, 216);
    REQUIRE(narrow.valid);
    CHECK(narrow.instruction.Bits(16, 8) == narrow.instruction.Bits(24, 8));
    CHECK(narrow.instruction.Bits(16, 8) < 216);
    load.words[2] |= 1U << 26; // Ra.64 would span R219-R220, two shadow quartets.
    const auto wide = Shader::Ampere::PrepareRegisterPressure(load, 216);
    REQUIRE(wide.valid);
    CHECK(wide.instruction.Bits(16, 8) == wide.instruction.Bits(24, 8));
    CHECK(wide.instruction.Bits(24, 8) + 2 <= 216);
}

TEST_CASE("Ampere conditional exits may end at a terminal self-branch", "[video_core][ampere]") {
    const Instruction conditional{{0x0000094d, 0, 0x03800000, 0}};
    const Instruction self{{0x00007947, 0xfffffff0, 0x0383ffff, 0}};
    const Instruction nop{{0x00007918, 0, 0, 0}};
    const auto lowered = LowerToMaxwell(std::array{conditional, self, nop});
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 32);
    CHECK(((Find(lowered, 0xe30, 52) >> 16) & 15) == 0); // EXIT remains conditional
    CHECK(((Find(lowered, 0xe24, 52) >> 16) & 15) == 7);
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{self, nop})));
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{conditional, nop})));
    const Instruction invalid_padding{{0x00007918, 1, 0, 0}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{conditional, self, invalid_padding})));
    // A forward branch still needs its captured target beyond the self-branch.
    const Instruction forward{{0x00001947, 0x30, 0x03800000, 0}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{forward, conditional, self, nop})));
}

TEST_CASE("Ampere LDG load predicates zero the result without accessing memory",
          "[video_core][ampere]") {
    // Interpret just the emitted control flow, immediate moves and LDG. Rd deliberately
    // aliases the address pair so zeroing before a taken load would corrupt its address.
    for (const bool uniform : {false, true}) {
        for (const u32 size : {0U, 1U, 2U, 3U, 4U, 5U, 6U}) {
            for (const u32 execution : {0U, 8U, 7U}) {
                for (const u32 condition : {0U, 9U, 7U, 15U}) {
                    for (const bool p0 : {false, true}) {
                        for (const bool p1 : {false, true}) {
                            CAPTURE(uniform, size, execution, condition, p0, p1);
                            const u32 encoded = (7U - (condition & 7)) | (condition & 8);
                            const std::array code{
                                Instruction{
                                    {(uniform ? 0xff080981U : 0x08080381U) | (execution << 12), 0,
                                     (uniform ? 0x081e8100U : 0x001e8100U) | (size << 9) | encoded,
                                     0}},
                                EXIT};
                            const auto lowered = LowerToMaxwell(code);
                            REQUIRE(static_cast<bool>(lowered));
                            const auto active = [&](u32 pred) {
                                const bool value = (pred & 7) == 7   ? true
                                                   : (pred & 7) == 0 ? p0
                                                                     : p1;
                                return value != bool(pred & 8);
                            };
                            std::array<u32, 256> registers{};
                            registers.fill(0x12345678);
                            registers[9] = 0;
                            registers[217] = 0;
                            const auto before = registers;
                            u32 loads = 0;
                            const u32 count = size <= 4 ? 1 : 1U << (size - 4);
                            const auto address = [](u32 i) {
                                return 0x50U + i * 8 + ((i + 1) / 3) * 8;
                            };
                            for (u32 pc = 0, steps = 0; pc < lowered.instructions.size(); ++pc) {
                                REQUIRE(++steps < 32);
                                const u64 word = lowered.instructions[pc];
                                if (!active((word >> 16) & 15))
                                    continue;
                                if ((word >> 52) == 0xe30)
                                    break;
                                if ((word >> 52) == 0xe24) {
                                    const s32 offset = static_cast<s32>((word >> 20) << 8) >> 8;
                                    const u32 target = address(pc) + 8 + offset;
                                    u32 next = 0;
                                    while (next < lowered.instructions.size() &&
                                           address(next) != target)
                                        ++next;
                                    REQUIRE(next < lowered.instructions.size());
                                    pc = next - 1;
                                } else if ((word >> 52) == 0x010) {
                                    registers[word & 0xff] = static_cast<u32>(word >> 20);
                                } else if ((word >> 56) == 0xee) {
                                    const u32 address_register = uniform ? 216 : 8;
                                    REQUIRE(((word >> 8) & 0xff) == address_register);
                                    CHECK(registers[address_register] == before[address_register]);
                                    CHECK(registers[address_register + 1] ==
                                          before[address_register + 1]);
                                    ++loads;
                                    for (u32 component = 0; component < count; ++component)
                                        registers[8 + component] = 0xabcdef00U + component;
                                } else {
                                    FAIL("Unexpected instruction in LDG predicate test");
                                }
                            }
                            CHECK(loads == (active(execution) && active(condition) ? 1U : 0U));
                            for (u32 component = 0; component < count; ++component) {
                                CHECK(registers[8 + component] ==
                                      (!active(execution)  ? before[8 + component]
                                       : active(condition) ? 0xabcdef00U + component
                                                           : 0U));
                            }
                        }
                    }
                }
            }
        }
    }
}
} // namespace
