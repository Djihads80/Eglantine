// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

class GatherEnvironment final : public Shader::Environment {
public:
    GatherEnvironment(std::span<const u64> instructions, u32 texture_binding)
        : binding{texture_binding}, code(0x50 / sizeof(u64)) {
        stage = Shader::Stage::Compute;
        start_address = 0x50;
        for (const u64 word : instructions) {
            if ((code.size() * sizeof(u64)) % 32 == 0) code.push_back(0);
            code.push_back(word);
        }
    }
    u64 ReadInstruction(u32 address) override {
        REQUIRE(address % sizeof(u64) == 0);
        REQUIRE(address / sizeof(u64) < code.size());
        return code[address / sizeof(u64)];
    }
    bool IsNativeProgram() const override { return true; }
    u32 ReadCbufValue(u32 bank, u32 offset) override { return (bank << 16) | offset; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return binding; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 4096; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    u32 binding;
    std::vector<u64> code;
};

Instruction LiteralOffset(u32 reg, u32 value, u32 guard) {
    return {{0xff000424U | (reg << 16) | (guard << 12), value, 0x078e00ff, 0}};
}

constexpr Instruction PTP_GATHER{{0x06189b63, 0x30007c10, 0x001e2f0e, 0}};

// Lowered Maxwell words whose top bits (above `shift`) are `opcode`.
template <typename Program>
size_t CountMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    return static_cast<size_t>(std::count_if(lowered.instructions.begin(),
                                             lowered.instructions.end(),
                                             [&](u64 word) { return (word >> shift) == opcode; }));
}

template <typename Program>
u64 FindMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    const auto it = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                 [&](u64 word) { return (word >> shift) == opcode; });
    return it != lowered.instructions.end() ? *it : 0;
}

TEST_CASE("Ampere BRA.CONV always branches and BRA.DIV never does", "[video_core][ampere]") {
    // BRA.CONV UR6 / BRA.DIV ~URZ to the second EXIT: the mode is in the offset's low two bits.
    const std::array<Instruction, 3> convergent{{
        {{0x06007947, 0x10 | 3, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(convergent[0], 0) == ProgramCaptureState::Result::Continue);
    const auto taken = LowerToMaxwell(convergent);
    REQUIRE(static_cast<bool>(taken));
    REQUIRE(CountMaxwell(taken, 0xe24, 52) == 1);

    const std::array<Instruction, 3> divergent{{
        {{0xff007947, 0x10 | 2, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    const auto dropped = LowerToMaxwell(divergent);
    REQUIRE(static_cast<bool>(dropped));
    REQUIRE(CountMaxwell(dropped, 0xe24, 52) == 0);
}

TEST_CASE("Ampere TLD.SCR.LZ takes its handle from a uniform register", "[video_core][ampere]") {
    // TLD.SCR.LZ R5, R8, R4, R5, UR4, 0x0, 2D, 0x7 -> Maxwell TLD.B.
    const std::array<Instruction, 2> code{{
        {{0x04087f66, 0x30000405, 0x089e0705, 0x001f4200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 tld = FindMaxwell(lowered, 0xdd, 56);
    REQUIRE(tld != 0);
    REQUIRE(((tld >> 28) & 7) == 2); // 2D
    REQUIRE(((tld >> 31) & 0xf) == 7);
}

TEST_CASE("Ampere I2F converts signed 16-bit sources", "[video_core][ampere]") {
    // I2F.S16 R8, R8
    const std::array<Instruction, 2> code{{
        {{0x00087306, 0x00000008, 0x00101400, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 i2f = FindMaxwell(lowered, 0x5cb, 52);
    REQUIRE(((i2f >> 10) & 3) == 1); // 16-bit source
    REQUIRE(((i2f >> 13) & 1) == 1); // signed
}

TEST_CASE("Ampere integer-to-float conversions take immediates", "[video_core][ampere]") {
    // I2F.U32.RP R7, 0x20; I2FP.F32.U32.RZ R4, 0x100; WARPSYNC R7
    const std::array<Instruction, 4> code{{
        {{0x00077906, 0x00000020, 0x00209000, 0x000e2200}},
        {{0x00047845, 0x00000100, 0x0020d000, 0x000fe200}},
        {{0x00007348, 0x00000007, 0x03800000, 0x000fe800}},
        EXIT,
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere SUST.P keeps its channel mask", "[video_core][ampere]") {
    // SUST.P.2D.STRONG.SM.R.IGN [R6], R4, 0x0, 0x59
    const std::array<Instruction, 2> code{{
        {{0x06007b9b, 0x60005904, 0x0010a100, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xeb2, 52) >> 20) & 0xf) == 1);
}

TEST_CASE("Ampere SHF.R.U64 immediate shifts keep their amount", "[video_core][ampere]") {
    // SHF.R.U64 R14, R14, n, R17 (Ocarina vertex shader 1a55fa57a335b963). The Maxwell
    // translator reads the immediate from bits 20-38, so no max-shift bits may be set there.
    for (const u32 shift : {0U, 1U, 21U, 31U, 32U}) {
        const std::array<Instruction, 2> code{{
            {{0x0e0e7819, shift, 0x00001211, 0x020fe200}},
            EXIT,
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u64 shf = FindMaxwell(lowered, 0x38f8, 48);
        REQUIRE(shf != 0);
        CHECK(((shf >> 20) & 0x7ffff) == shift);
        CHECK(((shf >> 39) & 0xff) == 17);
    }
}

TEST_CASE("Ampere SHFL with a register lane keeps its mode", "[video_core][ampere]") {
    // SHFL.DOWN PT, R7, R0, R5, 0x1f
    const std::array<Instruction, 2> code{{
        {{0x00077589, 0x08001f05, 0x000e0000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xef1, 52) >> 30) & 3) == 2);
}

TEST_CASE("Ampere calls find a return address set ahead of them", "[video_core][ampere]") {
    // MOV R9, 0x30; MOV R4, 0x5; CALL.REL.NOINC; EXIT; RET.REL.NODEC R9
    std::array<Instruction, 5> code{{
        {{0x00097802, 0x30, 0x00000f00, 0}},
        {{0x00047802, 0x5, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x09007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    // The link register is written again between its MOV and the call.
    code[1] = {{0x00097802, 0x5, 0x00000f00, 0}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere programs go on past an EXIT that a branch jumps over", "[video_core][ampere]") {
    // A call, then a branch over an EXIT to more of the program, with another call there.
    const std::array<Instruction, 8> code{{
        {{0x00047802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x50, 0x03c00000, 0}},
        {{0x00007947, 0x10, 0x03800000, 0}},
        EXIT,
        {{0x00047802, 0x60, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x04007950, 0xffffff80, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
}

TEST_CASE("Ampere capture ends at the return of a helper called past the EXIT",
          "[video_core][ampere]") {
    // Ocarina 69a40579971fb867: MOV R0, 0x20; CALL.REL.NOINC; EXIT; RET.REL.NODEC R0, then the
    // next program in memory.
    const std::array<Instruction, 4> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x00007950, 0xffffffc0, 0x03c3ffff, 0}},
    }};
    ProgramCaptureState capture;
    for (u32 index = 0; index + 1 < code.size(); ++index)
        CHECK(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(code.back(), 3) == ProgramCaptureState::Result::Complete);
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    ProgramCaptureState jump;
    CHECK(jump.Observe({{0x00007944, 0x10, 0x03c00000, 0}}, 0) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 1) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 2) == ProgramCaptureState::Result::Complete);
}

TEST_CASE("Ampere uniform select and bit operations reach every uniform register",
          "[video_core][ampere]") {
    // Ocarina e8decc3e5c0c184e: @UP0 USEL UR4, UR4, 0x20, UP1. UP0 and UP1 come from R239 into
    // P6 and P5; the SEL runs under P6 and the guest predicates are restored.
    const std::array<Instruction, 2> select{{
        {{0x04040887, 0x20, 0x08800000, 0x000fe200}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    const std::array<u64, 5> expected_select{
        0x010000000207f0fcULL, 0x38e800000607fffdULL, 0x38f000000607ef00ULL,
        0x5ca002800fc6dcdcULL, 0x38f000000607fd00ULL};
    CHECK(std::search(selected.instructions.begin(), selected.instructions.end(),
                      expected_select.begin(), expected_select.end()) != selected.instructions.end());

    // Ocarina 2079791ecb288b83: UBREV UR4, UR17.
    const std::array<Instruction, 2> reverse{{
        {{0x000472be, 0x11, 0x08000000, 0x000fc800}},
        EXIT,
    }};
    const auto reversed = LowerToMaxwell(reverse);
    INFO(reversed.error);
    REQUIRE(static_cast<bool>(reversed));
    CHECK(reversed.instructions.front() == 0x382801800007e9dcULL);

    // Ocarina 2079791ecb288b83: UFLO.U32.SH UR21, UR21.
    const std::array<Instruction, 2> leading{{
        {{0x001572bd, 0x15, 0x080e0400, 0x000fe400}},
        EXIT,
    }};
    const auto found = LowerToMaxwell(leading);
    INFO(found.error);
    REQUIRE(static_cast<bool>(found));
    CHECK(found.instructions.front() == 0x5c3002000ed700edULL);

    // Ocarina 2079791ecb288b83: UPOPC UR10, UR17.
    const std::array<Instruction, 2> count{{
        {{0x000a72bf, 0x11, 0x08000000, 0x000fe200}},
        EXIT,
    }};
    const auto counted = LowerToMaxwell(count);
    INFO(counted.error);
    REQUIRE(static_cast<bool>(counted));
    CHECK(counted.instructions.front() == 0x5c0800000e9700e2ULL);
}

TEST_CASE("Ampere guarded uniform comparisons keep the output when the guard is false",
          "[video_core][ampere]") {
    // Ocarina 2079791ecb288b83: @UP4 UISETP.GT.U32.AND UP0, UPT, UR21, 0x1f, UPT. UP4 and the old
    // UP0 load into P2 and P6, so a false guard merges UP0 back unchanged.
    const std::array<Instruction, 2> code{{
        {{0x1500488c, 0x1f, 0x0bf04070, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const std::array<u64, 6> expected{
        0x0100000001f7f0fcULL, 0x38e800000447fffdULL, 0x38f000000447ef00ULL,
        0x5b6803800fc2ed37ULL, 0x38e800000407efefULL, 0x38f000000447fd00ULL};
    CHECK(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));

    // SEL R3, R4, UR7, P0
    const std::array<Instruction, 2> select{{
        {{0x04037c07, 0x7, 0x08000000, 0x000fe400}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    CHECK(selected.instructions.front() == 0x5ca000000df70403ULL);
}

TEST_CASE("Ampere gathers take arrays, per-pixel offsets and depth references",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // Ocarina: TLD4.R.2D.ARRAY R44, R42, R8, R4. Layer and x in R8, R9, y in R4.
    const auto array = lower({{0x082c7b63, 0xb0801004, 0x001e0f2a, 0x00014200}});
    INFO(array.error);
    REQUIRE(static_cast<bool>(array));
    REQUIRE(array.instructions.size() >= 4);
    CHECK(array.instructions[0] == 0x5c980780008700faULL);
    CHECK(array.instructions[1] == 0x5c980780009700fbULL);
    CHECK(array.instructions[2] == 0x5c980780004700fcULL);
    CHECK(field(array.instructions[3], 20, 8) == 255);
    CHECK(field(array.instructions[3], 28, 3) == 3);
    CHECK(field(array.instructions[3], 50, 1) == 0);
    CHECK(field(array.instructions[3], 54, 2) == 0);

    // Ocarina: @!P1 TLD4.R.PTP R24, R14, R6, R16: x, y in R6, R7, the offsets in R16, R17.
    const auto per_pixel = lower({{0x06189b63, 0x30007c10, 0x001e2f0e, 0x000f4600}});
    INFO(per_pixel.error);
    REQUIRE(static_cast<bool>(per_pixel));
    REQUIRE(per_pixel.instructions.size() >= 5);
    CHECK(field(per_pixel.instructions[2], 20, 8) == 16);
    CHECK(field(per_pixel.instructions[3], 20, 8) == 17);
    CHECK(field(per_pixel.instructions[4], 20, 8) == 246);
    CHECK(field(per_pixel.instructions[4], 28, 3) == 2);
    CHECK(field(per_pixel.instructions[4], 54, 2) == 2);

    // Ocarina: TLD4.R.DC R12, R14, R24, R28: x, y in R24, R25, the reference in R28.
    const auto depth = lower({{0x180c7b63, 0x30800e1c, 0x001e4f0e, 0x000f4200}});
    INFO(depth.error);
    REQUIRE(static_cast<bool>(depth));
    REQUIRE(depth.instructions.size() >= 4);
    CHECK(field(depth.instructions[2], 20, 8) == 28);
    CHECK(field(depth.instructions[3], 50, 1) == 1);
    CHECK(field(depth.instructions[3], 54, 2) == 0);

    // Offset mode 3 is not an offset form.
    CHECK_FALSE(lower({{0x082c7b63, 0xb0801004, 0x001e3f2a, 0x00014200}}));
}

TEST_CASE("Ampere Ocarina sign extends, uniform shifts and carries, LEA.HI and SURED forms",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // SGXT R32, R32, 0x1: signed, so BFE.S32 (bit 48).
    const auto extend = lower({{0x2020781a, 1, 0x00000200, 0x000fe400}});
    INFO(extend.error);
    REQUIRE(static_cast<bool>(extend));
    CHECK(extend.instructions.front() == 0x3801000010072020ULL);

    // USHF.L.U32.HI UR5, URZ, UR4, 0x1: 1 << UR4 into UR5 (R221).
    const auto shift = lower({{0x3f057499, 1, 0x08010e04, 0x000fe400}});
    INFO(shift.error);
    REQUIRE(static_cast<bool>(shift));
    REQUIRE(shift.instructions.size() >= 2);
    CHECK(shift.instructions[1] == 0x5c4800800dc7faddULL);

    // UIADD3 UR6, UP2, UR15, UR6, URZ: the carry goes through P4 into R239 bit 4.
    const auto carry = lower({{0x0f067290, 6, 0x0ff5e03f, 0x000fe200}});
    INFO(carry.error);
    REQUIRE(static_cast<bool>(carry));
    const std::array<u64, 6> expected_carry{
        0x5c1000000de7e7faULL, 0x38e800000107fffdULL, 0x5b6203800e77fa27ULL,
        0x38e800000107efefULL, 0x38f000000107fd00ULL, 0x5c9807800fa700deULL};
    CHECK(std::equal(expected_carry.begin(), expected_carry.end(), carry.instructions.begin()));

    // LEA.HI R1, R1, -R0, RZ, 0x1e: the final add negates R0.
    const auto lea = lower({{0x01017211, 0x80000000, 0x078ff0ff, 0x000fca00}});
    INFO(lea.error);
    REQUIRE(static_cast<bool>(lea));
    REQUIRE(lea.instructions.size() >= 2);
    CHECK(lea.instructions[1] == 0x5c1100000007fa01ULL);

    // SURED.OR: op 6 at Maxwell bits 24-26.
    const auto reduce = lower({{0x0400159f, 0x60008400, 0x0310e000, 0x0001e200}});
    INFO(reduce.error);
    REQUIRE(static_cast<bool>(reduce));
    CHECK(field(reduce.instructions.front(), 24, 3) == 6);

    // LDG.E.128 R216, [R216.64] in a program using uniform registers: R216-R219 are reserved,
    // so the load goes through a borrowed low quartet.
    const std::array<Instruction, 3> load{{
        {{0x00007c02, 0, 0x08000f00, 0}},
        {{0xd8d87381, 0, 0x001ead00, 0x000f2800}},
        EXIT,
    }};
    const auto loaded = LowerToMaxwell(load);
    INFO(loaded.error);
    REQUIRE(static_cast<bool>(loaded));
    CHECK(std::any_of(loaded.instructions.begin(), loaded.instructions.end(), [&](u64 word) {
        return (word >> 52) == 0xeed && field(word, 0, 8) < 216 && field(word, 8, 8) < 216;
    }));
    // A reserved operand spanning two quartets (R218-R221) cannot be borrowed for.
    const std::array<Instruction, 3> straddle{{
        {{0x00007c02, 0, 0x08000f00, 0}},
        {{0xd8da7381, 0, 0x001ead00, 0x000f2800}},
        EXIT,
    }};
    CHECK_FALSE(LowerToMaxwell(straddle));
}

TEST_CASE("Ampere PTP literal proof respects register, predicate and control flow changes",
          "[video_core][ampere][gather]") {
    using Shader::Ampere::GatherOffsetProof;
    const std::array code{
        LiteralOffset(16, 0xfffeffff, 9), LiteralOffset(17, 0xfffcfffd, 9),
        Instruction{{0x1c1a7212, 0xff, 0x0784a003, 0}}, // LOP3 writes R26 and P2.
        Instruction{{0x0000281c, 0, 0x03f0f070, 0}}, // @P2 PLOP3 writes P0.
        Instruction{{0x00002947, 16, 0x03800000, 0}}, // @P2 exits, never enters the proof.
        PTP_GATHER, EXIT,
    };
    const auto expected = std::array<u32, 2>{0xfffeffff, 0xfffcfffd};
    REQUIRE(GatherOffsetProof{code}.Find(5) == expected);
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    const u32 first = lowered.source_instruction_offsets[5];
    CHECK(((lowered.instructions[first + 2] >> 16) & 15) == 7);
    CHECK(((lowered.instructions[first + 3] >> 16) & 15) == 7);
    CHECK(((lowered.instructions[first + 4] >> 16) & 15) == 9);

    for (const u32 definition : {0U, 1U}) {
        for (const u32 guard : {1U, 2U, 7U, 15U}) {
            auto changed = code;
            changed[definition].words[0] = (changed[definition].words[0] & ~0xf000U) | (guard << 12);
            CAPTURE(definition, guard);
            CHECK(GatherOffsetProof{changed}.Find(5).has_value() == (guard == 7));
        }
    }
    for (const u32 instruction : {2U, 3U}) {
        auto changed = code;
        changed[instruction].words[2] = (changed[instruction].words[2] & ~(7U << 17)) | (1U << 17);
        CHECK_FALSE(GatherOffsetProof{changed}.Find(5));
        changed[0] = LiteralOffset(16, 0xfffeffff, 7);
        changed[1] = LiteralOffset(17, 0xfffcfffd, 7);
        CHECK(GatherOffsetProof{changed}.Find(5) == expected);
    }
    for (const Instruction clobber : {
             Instruction{{0x04107810, 4, 0x07ffe0ff, 0}}, // IADD3 overwrites an offset.
             Instruction{{0x041c7810, 4, 0x07f3e0ff, 0}}, // IADD3 writes P1.
             Instruction{{0x000f7805, 0, 0x0001ff00, 0}}, // CS2R R15:R16 overlaps R16.
             Instruction{{0x00117919, 0, 0x2100, 0}}, // Dynamic S2R overwrites R17.
             Instruction{{0x00007949, 0, 0, 0}}, // Indirect control has unknown entry points.
             Instruction{{0x00007000, 0, 0, 0}}, // Unknown effects cannot prove constancy.
         }) {
        auto changed = code;
        changed[2] = clobber;
        CHECK_FALSE(GatherOffsetProof{changed}.Find(5));
    }
    auto changed = code;
    changed[4].words[1] = 0; // A branch directly to the gather bypasses the proof boundary.
    CHECK_FALSE(GatherOffsetProof{changed}.Find(5));
    changed[4].words[0] = 0x00007945; // Treat a BSSY reconvergence target conservatively too.
    CHECK_FALSE(GatherOffsetProof{changed}.Find(5));
    changed = code;
    changed[6] = {{0x00007947, static_cast<u32>(-80), 0x0383ffff, 0}}; // Back edge to instruction 2.
    CHECK_FALSE(GatherOffsetProof{changed}.Find(5));
    changed = code;
    changed[2] = {{0x00007000, 0, 0, 0}};
    changed[2].words[0] |= 0xf000; // @!PT cannot clobber anything.
    CHECK(GatherOffsetProof{changed}.Find(5) == expected);

    const std::array entry_at_definition{
        Instruction{{0x00007947, 0, 0x03800000, 0}},
        code[0], code[1], PTP_GATHER, EXIT,
    };
    CHECK(GatherOffsetProof{entry_at_definition}.Find(3) == expected);

    changed = code;
    changed[2] = PTP_GATHER;
    changed[2].words[2] = (changed[2].words[2] & ~0xffU) | 16U;
    CHECK_FALSE(GatherOffsetProof{changed}.Find(5)); // The gather's second destination overwrites R16.
    changed[2].words[2] |= 1U << 15;
    CHECK_FALSE(GatherOffsetProof{changed}.Find(5)); // F16 still writes its packed second destination.

    const auto dynamic = LowerToMaxwell(std::array{PTP_GATHER, EXIT});
    REQUIRE(dynamic);
    CHECK((dynamic.instructions[2] >> 48) == 0x5c98); // Unproven offsets still read guest registers.
    CHECK(((dynamic.instructions[2] >> 16) & 15) == 9);
}

TEST_CASE("Ampere guarded PTP literals survive production IR and SPIRV translation",
          "[video_core][ampere][gather]") {
    namespace IR = Shader::IR;
    // Regression for OOT's @!P1 gather. The literal rematerialization approach is also used by
    // Lichii 375c4a472bd95fdfaae0de6e3f192499cd14f806 (GPL-3.0-or-later).
    const std::array<u32, 8> expected{0xffffffff, 0xffffffff, 0xfffffffe, 0xffffffff,
                                     0xfffffffd, 0xffffffff, 0xfffffffc, 0xffffffff};
    for (const bool half : {false, true}) {
        for (const u32 guard : {1U, 7U, 9U}) {
            CAPTURE(half, guard);
            auto gather = PTP_GATHER;
            gather.words[0] = (gather.words[0] & ~0xf000U) | (guard << 12);
            gather.words[2] |= u32{half} << 15;
            const std::array native{
                Instruction{{0x000e7919, 0, 0x2100, 0}}, // Runtime thread index, so P1 is not constant.
                Instruction{{0x0e00720c, 0xff, 0x03f25070, 0}},
                LiteralOffset(16, 0xfffeffff, guard), LiteralOffset(17, 0xfffcfffd, guard),
                gather, EXIT,
            };
            const auto lowered = LowerToMaxwell(native);
            INFO(lowered.error);
            REQUIRE(lowered);
            auto code = lowered.instructions;
            // Shared stores keep all gather results alive through dead-code elimination.
            for (const u32 reg : {14U, 15U, 24U, 25U}) {
                code.insert(code.end() - 1, 0xef58000000070000ULL | reg | (255ULL << 8) |
                                           (u64{reg * 4} << 20) | (4ULL << 48));
            }
            GatherEnvironment env{code, lowered.required_texture_buffer};
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
            Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
            Shader::ObjectPool<IR::Inst> inst_pool;
            Shader::ObjectPool<IR::Block> block_pool;
            Shader::HostTranslateInfo host{};
            host.ApplyDescriptorLimitPolicy();
            auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
            u32 gathers = 0;
            for (const auto* block : program.blocks) {
                for (const auto& inst : *block) {
                    if (inst.GetOpcode() != IR::Opcode::ImageGather) continue;
                    ++gathers;
                    for (u32 vector = 0; vector < 2; ++vector) {
                        const auto* offsets = inst.Arg(2 + vector).InstRecursive();
                        REQUIRE(offsets->GetOpcode() == IR::Opcode::CompositeConstructU32x4);
                        REQUIRE(offsets->AreAllArgsImmediates());
                        for (u32 lane = 0; lane < 4; ++lane)
                            CHECK(offsets->Arg(lane).U32() == expected[vector * 4 + lane]);
                    }
                }
            }
            REQUIRE(gathers == 1);
            Shader::Profile profile{};
            // Device::SupportedSpirvVersion selects 1.6 for Vulkan >= 1.3, including the RTX 5070.
            // Validate these modules with spirv-val --target-env vulkan1.3.
            profile.supported_spirv = 0x00010600;
            const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            REQUIRE(spirv.size() > 5);
            CHECK(spirv.front() == 0x07230203);
            CHECK(spirv[1] == profile.supported_spirv);
            std::map<u32, u32> scalars;
            std::map<u32, std::vector<u32>> composites;
            std::vector<u32> offset_arrays;
            for (size_t i = 5; i < spirv.size();) {
                const u32 count = spirv[i] >> 16, opcode = spirv[i] & 0xffff;
                REQUIRE(count > 0);
                REQUIRE(i + count <= spirv.size());
                if (opcode == 43 && count == 4) scalars[spirv[i + 2]] = spirv[i + 3]; // OpConstant
                if (opcode == 44) // OpConstantComposite
                    composites[spirv[i + 2]] = {spirv.begin() + i + 3, spirv.begin() + i + count};
                if (opcode == 96) { // OpImageGather must carry ConstOffsets, not silently drop them.
                    REQUIRE(count == 8);
                    CHECK(spirv[i + 6] == 0x20);
                    offset_arrays.push_back(spirv[i + 7]);
                }
                i += count;
            }
            REQUIRE(offset_arrays.size() == 1);
            const auto& taps = composites.at(offset_arrays.front());
            REQUIRE(taps.size() == 4);
            for (u32 tap = 0; tap < 4; ++tap) {
                const auto& pair = composites.at(taps[tap]);
                REQUIRE(pair.size() == 2);
                CHECK(scalars.at(pair[0]) == expected[tap * 2]);
                CHECK(scalars.at(pair[1]) == expected[tap * 2 + 1]);
            }
            // Optional artifacts for spirv-val; normal test execution writes no files.
            if (const char* directory = std::getenv("EDEN_TEST_SPIRV_DIR")) {
                const auto folder = std::filesystem::path{directory};
                std::filesystem::create_directories(folder);
                std::ofstream output{folder / fmt::format("gather-{}-{}.spv", guard, half),
                                     std::ios::binary};
                output.write(reinterpret_cast<const char*>(spirv.data()),
                             static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
                REQUIRE(output.good());
            }
        }
    }
}
} // namespace
