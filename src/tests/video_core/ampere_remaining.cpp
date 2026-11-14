// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
}

TEST_CASE("Ampere large captured programs keep bounded size validation", "[ampere]") {
    std::vector<Instruction> code(82720, Instruction{{0x00007918, 0, 0, 0}});
    code.back() = Exit;
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == code.size() * 16);
    code.resize(131073, code.front());
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere texture spans cover native scratch aliases", "[ampere]") {
    for (const Instruction native : {
        Instruction{{0x42022b60, 0x30801241, 0x041e07da, 0}},
        Instruction{{0x00e47b60, 0xe0801ce4, 0x041e41ff, 0}},
        Instruction{{0x9cee7f60, 0x3000069d, 0x089e02ff, 0}}}) {
        const auto pressure = Shader::Ampere::PrepareRegisterPressure(native, 216);
        REQUIRE(pressure.valid);
        REQUIRE_FALSE(pressure.prefix.empty());
        CHECK(pressure.prefix.size() == pressure.suffix.size());
    }
}

TEST_CASE("Ampere native atomics preserve exchange reduction types and validation", "[ampere]") {
    const Instruction exchange{{0xffff79a8, 0x000004ff, 0x0c1ee144, 0}};
    const auto lowered = LowerToMaxwell(std::array{exchange, Exit});
    REQUIRE(static_cast<bool>(lowered));
    bool found = false;
    for (u64 w : lowered.instructions) if ((w >> 56) == 0xed) {
        found = true;
        CHECK(((w >> 52) & 15) == 8);
        CHECK(((w >> 28) & 0xfffff) == 4);
    }
    REQUIRE(found);
    const Instruction reduction{{0x0600798e, 0x13, 0x0010e900, 0}};
    const auto red = LowerToMaxwell(std::array{reduction, Exit});
    REQUIRE(static_cast<bool>(red));
    CHECK(((red.instructions.front() >> 20) & 7) == 4);
    for (u32 flag : {0x200U, 0x400U, 0x800000U}) {
        auto invalid = Instruction{{0x060873a9, 0x0a, 0x001ee10b, 0}};
        invalid.words[2] ^= flag;
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{invalid, Exit})));
    }
}

TEST_CASE("Ampere general texel fetch preserves coordinates LOD handles and split results",
          "[video_core][ampere]") {
    for (const u32 opcode : {0xb66U, 0xf66U, 0x367U}) {
        for (const u32 dimension : {1U, 2U}) {
            for (const bool scalar : {false, true}) {
                for (const u32 mask : {1U, 7U, 15U}) {
                    const bool register_handle = opcode == 0x367, uniform_handle = opcode == 0xf66;
                    const u32 coordinates = dimension == 2 ? 3 : 2;
                    if (opcode == 0xb66 && dimension == 1 && mask == 1) continue; // Existing TLDS form.
                    const u32 count = coordinates + 1 + (register_handle ? 1 : 0);
                    if (scalar && count > 4) continue;
                    const u32 in_a = scalar ? (count + 1) / 2 : coordinates;
                    // Odd Rd forces the bound form through general TLD rather than TLDS.
                    const u32 dest = 5, second = mask == 1 ? 255U : 9U;
                    Instruction native{{0x10007000U | (dest << 16) | opcode,
                        24U | (dimension << 29) | (scalar ? 1U << 28 : 0U) |
                            (register_handle ? 1U << 27 : uniform_handle ? 0U : 0x00c00400U),
                        0x019e0000U | (mask << 8) | second | (uniform_handle ? 1U << 27 : 0U), 0}};
                    const auto result = LowerToMaxwell(std::array{native, Exit});
                    REQUIRE(static_cast<bool>(result));
                    std::array<u32, 256> regs{};
                    for (u32 i = 0; i < 255; ++i) regs[i] = 0x1000 + i;
                    regs[216] = 0xabc; // UR0 descriptor mirror for 0xf66.
                    const auto source = [&](u32 i) { return 0x1000 + (i < in_a ? 16 + i : 24 + i - in_a); };
                    bool fetched = false;
                    for (u64 word : result.instructions) {
                        if ((word >> 56) == 0xdc || (word >> 56) == 0xdd) {
                            REQUIRE_FALSE(fetched);
                            fetched = true;
                            const bool bindless = (word >> 56) == 0xdd;
                            CHECK(bindless == (uniform_handle || register_handle));
                            CHECK(((word >> 28) & 7) == (dimension == 2 ? 4U : 2U));
                            CHECK(((word >> 55) & 1) == 1);
                            CHECK(((word >> 31) & 15) == mask);
                            const u32 coord = (word >> 8) & 255, meta = (word >> 20) & 255;
                            for (u32 i = 0; i < coordinates; ++i) CHECK(regs[coord + i] == source(i));
                            if (register_handle) CHECK(regs[meta] == source(coordinates));
                            if (uniform_handle) CHECK(regs[meta] == 0xabc);
                            CHECK(regs[meta + bindless] == source(coordinates + register_handle));
                            for (u32 i = 0; i < static_cast<u32>(std::popcount(mask)); ++i) regs[(word & 255) + i] = 0xfeed0000 + i;
                        } else if ((word >> 52) == 0x5c9) {
                            regs[word & 255] = regs[(word >> 20) & 255];
                        }
                    }
                    REQUIRE(fetched);
                    const u32 components = std::popcount(mask);
                    for (u32 i = 0; i < components; ++i)
                        CHECK(regs[i < 2 ? dest + i : second + i - 2] == 0xfeed0000 + i);
                }
            }
        }
    }
}

TEST_CASE("Ampere general texel fetch rejects clamp and invalid source spans", "[video_core][ampere]") {
    // Captured 3D.LL failure, with the same source layout confirmed by ptxas SM86.
    Instruction native{{0x02027b66, 0x50c01608, 0x019e01ff, 0x000f4200}};
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})));
    native.words[2] |= 1U << 13; // .CL has additional semantics.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})));
    native.words[2] &= ~(1U << 13);
    native.words[1] = (native.words[1] & ~255U) | 254U;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})));
}

TEST_CASE("Ampere SEL, constant IMAD and SUST shadow reserved native registers", "[video_core][ampere]") {
    // Captured: SEL R250, R250, RZ, P4 (Daemon X Machina), IMAD R249, R249, c[0x3][0x0], RZ
    // (WILD HEARTS S) and SUST.P.1D_BUFFER.IGN [R252], R248 with four channels (Daemon X Machina).
    const auto lowered_registers = [](Instruction native, u64 top, u64 top_mask) {
        const auto result = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(result));
        const auto word = std::ranges::find_if(result.instructions, [&](u64 w) { return (w & top_mask) == top; });
        REQUIRE(word != result.instructions.end());
        return *word;
    };
    const u64 sel = lowered_registers({{0xfafa7207, 0x000000ff, 0x02000000, 0x000fe400}},
                                      0x5ca0000000000000ULL, 0xfff8000000000000ULL);
    CHECK((sel & 255) < 244);
    CHECK(((sel >> 8) & 255) == (sel & 255));
    CHECK(((sel >> 20) & 255) == 255);
    CHECK(((sel >> 39) & 15) == 4);
    const u64 sust = lowered_registers({{0xfc007b9b, 0x20c002f8, 0x00100f00, 0x0001e200}},
                                       0xeb20000000000000ULL, 0xfff0000000000000ULL);
    CHECK((sust & 255) < 244);
    CHECK((sust & 3) == 0);
    CHECK(((sust >> 8) & 255) < 244);
    CHECK(((sust >> 20) & 15) == 15);
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{
        Instruction{{0xf9f97a24, 0x00c00000, 0x078e02ff, 0x000fe200}}, Exit})));
    // Four channels from R246 borrow two adjacent quartets.
    const auto straddling = LowerToMaxwell(std::array{
        Instruction{{0xfc007b9b, 0x20c002f6, 0x00100f00, 0x0001e200}}, Exit});
    REQUIRE(static_cast<bool>(straddling));
    const auto store = std::ranges::find_if(straddling.instructions, [](u64 word) {
        return (word & 0xfff0000000000000ULL) == 0xeb20000000000000ULL;
    });
    REQUIRE(store != straddling.instructions.end());
    CHECK((*store & 255) + 4 <= 244);
}

TEST_CASE("Ampere LDL with a uniform base reads [URn + offset]", "[video_core][ampere]") {
    // Captured Drag x Drive LDL.128 R128, [UR57+0x340] (ff807983 00034039 08100c00), with UR5
    // (mirrored in R221) so no high uniform bank is needed.
    const Instruction native{{0xff807983, 0x00034005, 0x08100c00, 0x000ea200}};
    const auto lowered = LowerToMaxwell(std::array{native, Exit});
    REQUIRE(static_cast<bool>(lowered));
    u32 loads = 0;
    for (const u64 word : lowered.instructions) {
        if ((word >> 52) != 0xef4) continue;
        CHECK((word & 255) == 128 + loads);
        CHECK(((word >> 8) & 255) == 221);
        CHECK(((word >> 20) & 0xffffff) == 0x340 + 4 * loads);
        CHECK(((word >> 48) & 7) == 4);
        ++loads;
    }
    CHECK(loads == 4);
    Instruction register_base = native; // Bit 91 with a register base is not this form.
    register_base.words[0] = 0x04807983;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{register_base, Exit})));
    Instruction store = native; // STL keeps its data register in bits 32-39.
    store.words[0] = 0xff007387;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{store, Exit})));
}

TEST_CASE("Ampere surface accesses with system-scope strong order are fenced", "[video_core][ampere]") {
    // Captured Stellar Blade: SULD.P.1D_BUFFER.STRONG.SYS.R.IGN R7, [R7], R8 (07077998 20000000
    // 001f4108) and SUST.P.1D_BUFFER.STRONG.SYS.IGN [R16], R8, R6 (1000799c 20000008 00114f06).
    // Bits 77-80 are Mesa NAK's SM80 memory order, 0xa = strong system.
    for (const Instruction native : {Instruction{{0x07077998, 0x20000000, 0x001f4108, 0x008f6400}},
                                     Instruction{{0x1000799c, 0x20000008, 0x00114f06, 0x0041e200}}}) {
        const auto lowered = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(lowered));
        const auto access = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word >> 52) == 0xeb0 || (word >> 52) == 0xeb2;
        });
        REQUIRE(access != lowered.instructions.end());
        REQUIRE(access != lowered.instructions.begin());
        CHECK(*(access - 1) == 0xef98000000070200ULL); // MEMBAR.SYS before...
        CHECK(*(access + 1) == 0xef98000000070200ULL); // ...and after.
        Instruction weaker = native; // Strong GPU (order 7) is not accepted by these forms.
        weaker.words[2] ^= (0xaU ^ 0x7U) << 13;
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{weaker, Exit})));
    }
}

TEST_CASE("Ampere SEL immediate and STG shadow reserved native registers", "[video_core][ampere]") {
    // Captured WILD HEARTS S: STG.E.STRONG.SM [R102], R246 (66007386 000000f6 0010a900).
    const auto stg = LowerToMaxwell(std::array{Instruction{{0x66007386, 0x000000f6, 0x0010a900, 0}}, Exit});
    REQUIRE(static_cast<bool>(stg));
    const auto store = std::ranges::find_if(stg.instructions, [](u64 word) { return (word >> 52) == 0xeed; });
    REQUIRE(store != stg.instructions.end());
    CHECK((*store & 255) < 244);
    CHECK(((*store >> 8) & 255) == 102);
    // SEL R250, RZ, 0xffffffff, P5 in a high register.
    const auto sel = LowerToMaxwell(std::array{Instruction{{0xfffa7807, 0xffffffff, 0x02800000, 0}}, Exit});
    REQUIRE(static_cast<bool>(sel));
}

TEST_CASE("Ampere ARRAY_2D depth-compare TEX from other descriptor buffers keeps its operands",
          "[video_core][ampere]") {
    // Captured Welcome Tour: TEX.SCR.DC.NODEP RZ, R34, R32, R34, 0x4, 0x124, ARRAY_2D, 0x1
    // (20227b60 b1012422 041e41ff), then a bank-9 one, which a second buffer reads bindless.
    const Instruction bank4{{0x20227b60, 0xb1012422, 0x041e41ff, 0x000fc200}};
    const Instruction bank9{{0x14147b60, 0xb241381e, 0x041e41ff, 0x00814400}};
    const auto lowered = LowerToMaxwell(std::array{bank4, bank9, Exit});
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.required_texture_buffer == 4);
    std::array<u32, 256> regs{};
    for (u32 i = 0; i < 255; ++i) regs[i] = 0x1000 + i;
    std::vector<u64> samples;
    for (const u64 word : lowered.instructions) {
        if ((word >> 52) == 0x5c9) regs[word & 255] = regs[(word >> 20) & 255];
        if ((word >> 52) == 0x4c9) {
            CHECK(((word >> 34) & 31) == 9);
            regs[word & 255] = 0xdead0000U | static_cast<u32>((word >> 20) & 0x3fff);
        }
        if ((word >> 56) == 0xc0 || (word >> 55) == (0xde80000000000000ULL >> 55)) {
            const bool tex_b = (word >> 56) == 0xde;
            CHECK(((word >> 28) & 7) == 3);           // ARRAY_2D
            CHECK(((word >> 50) & 1) == 1);           // DC
            CHECK(((word >> 31) & 15) == 1);
            const u32 coord = (word >> 8) & 255, meta = (word >> 20) & 255;
            const u32 a = samples.empty() ? 32 : 20, b = samples.empty() ? 34 : 30;
            CHECK(regs[coord] == 0x1000 + a);         // Array index
            CHECK(regs[coord + 1] == 0x1000 + a + 1); // x
            CHECK(regs[coord + 2] == 0x1000 + b);     // y
            if (tex_b) {
                // The handle, loaded from bank 9 at the same word offset, is the first meta value.
                CHECK(regs[meta] == (0xdead0000U | 0x138));
                CHECK(((word >> 36) & 3) == 0); // No AOFFI, no LOD.
                CHECK(((word >> 49) & 1) == 1); // .NODEP
            } else {
                CHECK(((word >> 36) & 0x1fff) == 0x124); // Word offsets in both encodings.
            }
            CHECK(regs[meta + tex_b] == 0x1000 + b + 1); // Depth reference
            CHECK(tex_b == !samples.empty());
            samples.push_back(word);
        }
    }
    CHECK(samples.size() == 2);
}

TEST_CASE("Ampere TMML and TXQ read a second descriptor buffer bindless", "[video_core][ampere]") {
    // Captured Welcome Tour: TMML.LOD.NODEP RZ, R21, R0, 0xb, 0x0, 2D, 0x2 (00157b69 22c000ff
    // 040002ff) and TXQ RZ, R2, R7, TEX_HEADER_DIMENSION, 0xb, 0xe0, 0x3 (07027b6f 02c0e000 000003ff),
    // each after an access to another bank.
    const auto bank = [](Instruction insn, u32 binding) {
        insn.words[1] = (insn.words[1] & ~(31U << 22)) | (binding << 22);
        return insn;
    };
    const Instruction tmml{{0x00157b69, 0x22c000ff, 0x040002ff, 0x000f4200}};
    const Instruction txq{{0x07027b6f, 0x02c0e000, 0x000003ff, 0x000f4200}};
    for (const bool query_size : {false, true}) {
        const Instruction native = query_size ? txq : tmml;
        const u32 handle_reg = query_size ? 252 : 246;
        // Maxwell TXQ / TXQ.B and TMML / TMML.B (top 13 bits).
        const u64 bound_form = (query_size ? 0xdf48000000000000ULL : 0xdf58000000000000ULL) >> 51;
        const u64 handle_form = (query_size ? 0xdf50000000000000ULL : 0xdf60000000000000ULL) >> 51;
        const auto lowered = LowerToMaxwell(std::array{bank(native, 4), native, Exit});
        REQUIRE(static_cast<bool>(lowered));
        CHECK(lowered.required_texture_buffer == 4);
        bool loaded = false, bound = false, bindless = false;
        for (const u64 word : lowered.instructions) {
            if ((word >> 52) == 0x4c9 && (word & 255) == handle_reg) {
                CHECK(((word >> 34) & 31) == 11);                                // Bank 11...
                CHECK(((word >> 20) & 0x3fff) == ((native.words[1] >> 8) & 0x3fff)); // ...same word.
                loaded = true;
            }
            if ((word >> 51) == bound_form) bound = true;
            if ((word >> 51) == handle_form) {
                // TXQ.B takes the handle in Ra, TMML.B in Rb; neither keeps an offset.
                bindless = true;
                CHECK(((word >> (query_size ? 8 : 20)) & 255) == handle_reg);
                CHECK(((word >> 36) & 0x1fff) == 0);
            }
        }
        CHECK(loaded);
        CHECK(bound);
        CHECK(bindless);
    }
}

TEST_CASE("Ampere register-relative local stores take signed offsets", "[video_core][ampere]") {
    // Captured Welcome Tour ray tracing: STL.128 [R8+-0x160], R12 (08007387 fffea00c 00100c00).
    const auto lowered = LowerToMaxwell(std::array{
        Instruction{{0x08007387, 0xfffea00c, 0x00100c00, 0x0001e800}}, Exit});
    REQUIRE(static_cast<bool>(lowered));
    u32 stores = 0;
    for (const u64 word : lowered.instructions) {
        if ((word >> 52) != 0xef5) continue;
        CHECK((word & 255) == 12 + stores);
        CHECK(((word >> 8) & 255) == 8);
        const s32 offset = static_cast<s32>(static_cast<u32>(word >> 20) << 8) >> 8;
        CHECK(offset == -0x160 + 4 * static_cast<s32>(stores));
        CHECK(((word >> 44) & 15) == 0); // Nothing spills past the 24-bit field.
        ++stores;
    }
    CHECK(stores == 4);
    // [RZ + offset] stays an absolute, unsigned address.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{
        Instruction{{0xff007387, 0xfffea00c, 0x00100c00, 0x0001e800}}, Exit})));
}

TEST_CASE("Ampere F2F.F16.F32 reads a constant-buffer source with its modifiers", "[video_core][ampere]") {
    // Captured Welcome Tour: F2F.F16.F32 R19, -c[0x3][0x40] (00137b04 80c01000 00200800); ptxas
    // emits the same form for cvt.rn.f16.f32 of a negated parameter.
    for (const u32 modifiers : {0U, 1U, 2U, 3U}) {
        const Instruction native{{0x00137b04, 0x00c01000U | (modifiers << 30), 0x00200800, 0x001fe200}};
        const auto lowered = LowerToMaxwell(std::array{native, Exit});
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() >= 2);
        const u64 load = lowered.instructions[0], convert = lowered.instructions[1];
        CHECK((load >> 52) == 0x4c9);
        CHECK((load & 255) == 250);
        CHECK(((load >> 34) & 31) == 3);
        CHECK(((load >> 20) & 0x3fff) == 0x40 / 4);
        CHECK((convert >> 51) == (0x5ca8ULL >> 3)); // F2F (reg)
        CHECK((convert & 255) == 19);
        CHECK(((convert >> 20) & 255) == 250);
        CHECK(((convert >> 8) & 3) == 1);  // F16 result
        CHECK(((convert >> 10) & 3) == 2); // F32 source
        CHECK(((convert >> 49) & 1) == (modifiers & 1)); // |.| (bit 62)
        CHECK(((convert >> 45) & 1) == (modifiers >> 1)); // - (bit 63)
    }
}

TEST_CASE("Ampere IPA's attribute field is not a register operand", "[video_core][ampere]") {
    // Captured Welcome Tour: IPA.PASS R9, a[0x3b8] (00097326 000000ff 000e00ee): bits 64-71 are
    // the attribute address >> 2 (Mesa NAK), which looks like R238.
    const auto lowered = LowerToMaxwell(std::array{
        Instruction{{0x00097326, 0x000000ff, 0x000e00ee, 0x000e2200}}, Exit});
    INFO(lowered.error);
    CHECK(static_cast<bool>(lowered));
}
