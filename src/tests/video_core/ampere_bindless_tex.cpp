// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr std::array<Instruction, 3> sample{{
    {{0x3f057899, 0x14, 0x08011604, 0x000fd200}},
    {{0x02007f60, 0x30000403, 0x0c9e0f02, 0x101f4200}},
    {{0x0000794d, 0, 0x03800000, 0}},
}};

u64 Copy(u32 target, u32 source, u32 predicate = 7) {
    return 0x5c98000000000000ULL | (u64{predicate} << 16) | target | (u64{15} << 39) |
           (u64{source} << 20);
}

Instruction Branch(s32 relative, u32 predicate = 7) {
    const u64 raw = static_cast<u64>(static_cast<s64>(relative));
    return {{0x947U | (predicate << 12), static_cast<u32>(raw),
             0x03800000U | static_cast<u32>((raw >> 32) & 0x3ffff), 0}};
}

class TextureEnvironment final : public Shader::Environment {
public:
    explicit TextureEnvironment(std::span<const u64> instructions_) : instructions{instructions_} {}

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return instructions[i];
        }
        FAIL("Read outside lowered instructions");
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 2);
        REQUIRE((offset == 0x168 || offset == 0x170 || offset == 0x568));
        return offset == 0x568 ? 0x12300000U : offset == 0x170 ? 0xbcdefU : 0xabcdeU;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        REQUIRE((handle == 0x123abcde || handle == 0x123bcdef));
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE((handle == 0x123abcde || handle == 0x123bcdef));
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE((handle == 0x123abcde || handle == 0x123bcdef));
        return false;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override {
        FAIL("Bindless TEX must not use the bound texture buffer");
        return 0;
    }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> instructions;
};

TEST_CASE("Ampere uniform bindless TEX traces both merged cbuf handle sources",
          "[video_core][ampere][bindless_tex]") {
    const u32 lod = GENERATE(0U, 1U);
    const u32 mask = GENERATE(1U, 3U, 15U);
    const u32 nodep = GENERATE(0U, 1U);
    std::array<Instruction, 33> code{{
        {{0x00047ab9, 0x00805a00, 0x00000800, 0x000fe400}},
        {{0x00057ab9, 0x00815a00, 0x00000800, 0x000fe400}},
        {{0x04047292, 0x00000005, 0x0f8efc3f, 0x000fc800}},
        sample[0],
        {{0x00107308, 0x00000010, 0x00001000, 0x001e6400}},
        {{0x10137220, 0x00000013, 0x0041c000, 0x042fe200}},
        {{0x10127220, 0x00000015, 0x0041c000, 0x000fc600}},
        {{0x13157a20, 0x01402100, 0x00410000, 0x000fc800}},
        {{0x12157a23, 0x01402000, 0x00010015, 0x000fc800}},
        {{0x15147621, 0x01402200, 0x00010000, 0x000fe200}},
        {{0x13157a20, 0x01401d00, 0x00410000, 0x040fe200}},
        {{0x13137a20, 0x01401900, 0x00410000, 0x000fc600}},
        {{0x12157a23, 0x01401c00, 0x00010015, 0x040fe200}},
        {{0x00147308, 0x00000014, 0x00001000, 0x000e2200}},
        {{0x12137a23, 0x01401800, 0x00010013, 0x000fe400}},
        {{0x15157621, 0x01401e00, 0x00010000, 0x000fe400}},
        {{0x13137621, 0x01401a00, 0x00010000, 0x000fe400}},
        {{0x15127220, 0x00000014, 0x00410000, 0x081fe400}},
        {{0x13137220, 0x00000014, 0x00410000, 0x000fc400}},
        {{0x01017220, 0x00000012, 0x00410000, 0x084fe200}},
        {{0x05057220, 0x00000012, 0x00410000, 0x008fc600}},
        {{0x00007223, 0x00000013, 0x00010001, 0x080fe200}},
        {{0x04047223, 0x00000013, 0x00010005, 0x000fc600}},
        {{0x03007221, 0x00000000, 0x00010000, 0x000fe200}},
        {{0x07077221, 0x00000004, 0x00010000, 0x000fc600}},
        {{0x09097220, 0x00000000, 0x00410000, 0x090fe200}},
        {{0x0d0d7220, 0x00000000, 0x00410000, 0x000fc600}},
        {{0x08097223, 0x00000007, 0x00010009, 0x080fe200}},
        {{0x0c057223, 0x00000007, 0x0001000d, 0x000fc600}},
        {{0x0a047221, 0x00000009, 0x00010000, 0x000fe200}},
        {{0x0e057221, 0x00000005, 0x00010000, 0x000fcc00}},
        {{0x04047f60, 0x30000405, 0x0c1e0f06, 0x100f4200}},
        sample[2],
    }};
    code[31].words[2] = 0x081e0000U | (nodep << 26) | (lod << 23) | (mask << 8) |
                        (mask == 15 ? 6U : 255U);
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((lowered.instructions[lowered.source_instruction_offsets[31] + 2] >> 49) & 1) == nodep);
    REQUIRE(lowered.source_size == sizeof(code));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    TextureEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    const u32 last = static_cast<u32>(lowered.instructions.size() - 1);
    Shader::Maxwell::Translate(env, &block, 0x50, 0x50 + last * 8 + ((last + 1) / 3) * 8);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host_info);
    const auto expected = lod ? Shader::IR::Opcode::ImageSampleExplicitLod
                              : Shader::IR::Opcode::ImageSampleImplicitLod;
    REQUIRE(std::count_if(block.begin(), block.end(), [expected](const Shader::IR::Inst& inst) {
                return inst.GetOpcode() == expected;
            }) == 1);
    REQUIRE(program.info.texture_descriptors.size() == 1);
    const auto& descriptor = program.info.texture_descriptors.front();
    REQUIRE(descriptor.type == Shader::TextureType::Color2D);
    REQUIRE_FALSE(descriptor.is_depth);
    REQUIRE(descriptor.has_secondary);
    REQUIRE(descriptor.cbuf_index == 2);
    REQUIRE(descriptor.cbuf_offset == 0x168);
    REQUIRE(descriptor.secondary_cbuf_index == 2);
    REQUIRE(descriptor.secondary_cbuf_offset == 0x568);
    REQUIRE(descriptor.shift_left == 0);
    REQUIRE(descriptor.secondary_shift_left == 0);
    REQUIRE(descriptor.count == 1);
}

class MixedFetchEnvironment final : public Shader::Environment {
public:
    MixedFetchEnvironment(std::span<const u64> instructions_, Shader::TextureType type_)
        : instructions{instructions_}, type{type_} {}

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return instructions[i];
        }
        FAIL("Read outside mixed-fetch instructions");
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE((index == 2 || index == 4));
        REQUIRE(offset == 0x600);
        return index == 2 ? 0x20 : 0x40;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        REQUIRE((handle == 0x20 || handle == 0x40));
        return handle == 0x20 ? Shader::TextureType::Color2D : type;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE((handle == 0x20 || handle == 0x40));
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE((handle == 0x20 || handle == 0x40));
        return false;
    }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> instructions;
    Shader::TextureType type;
};

TEST_CASE("Ampere mixed-cbuf F32 texel fetches retain each descriptor and split output",
          "[video_core][ampere][bindless_tex]") {
    const u32 dimension = GENERATE(0U, 1U, 2U, 5U);
    const u32 mask = GENERATE(1U, 3U, 9U, 7U, 15U);
    const u32 nodep = GENERATE(0U, 1U);
    const u32 count = static_cast<u32>(std::popcount(mask));
    const u32 second = count > 2 ? 20U : 255U;
    std::array<Instruction, 3> code{{
        {{0x26267b66, 0x3081802d, 0x049e01ff, 0x004f4a00}},
        {{0x08087b66, 0x1101800a | (dimension << 29),
          0x009e0000 | (nodep << 26) | (mask << 8) | second, 0x004f4a00}},
        sample[2],
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 2);
    const u32 begin = lowered.source_instruction_offsets[1];
    REQUIRE(lowered.instructions[begin] == Copy(250, 8));
    const u32 coordinates = dimension == 0 ? 1U : dimension == 1 ? 2U : 3U;
    if (dimension == 1) REQUIRE(lowered.instructions[begin + 1] == Copy(251, 10));
    if (coordinates == 3) {
        REQUIRE(lowered.instructions[begin + 1] == Copy(251, 9));
        REQUIRE(lowered.instructions[begin + 2] == Copy(252, 10));
    }
    REQUIRE(lowered.instructions[begin + coordinates] ==
            (0x4c98000000070000ULL | 246ULL | (15ULL << 39) |
             (0x180ULL << 20) | (4ULL << 34)));
    const u64 fetch = lowered.instructions[begin + coordinates + 1];
    REQUIRE((fetch >> 56) == 0xdd);
    REQUIRE(((fetch >> 49) & 1) == nodep);
    REQUIRE(((fetch >> 31) & 15) == mask);
    for (u32 i = 0; i < count; ++i) {
        REQUIRE(lowered.instructions[begin + coordinates + 2 + i] ==
                Copy(i >= 2 ? second + i - 2 : 8 + i, 250 + i));
    }
    const auto type = dimension == 0 ? Shader::TextureType::Color1D :
                      dimension == 1 ? Shader::TextureType::Color2D :
                      dimension == 2 ? Shader::TextureType::Color3D :
                                       Shader::TextureType::ColorArray2D;
    MixedFetchEnvironment env{lowered.instructions, type};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    const u32 last = static_cast<u32>(lowered.instructions.size() - 1);
    Shader::Maxwell::Translate(env, &block, 0x50, 0x50 + last * 8 + ((last + 1) / 3) * 8);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host_info);
    REQUIRE(program.info.texture_descriptors.size() == 2);
    REQUIRE(program.info.texture_descriptors[0].cbuf_index == 2);
    REQUIRE(program.info.texture_descriptors[0].cbuf_offset == 0x600);
    REQUIRE(program.info.texture_descriptors[1].cbuf_index == 4);
    REQUIRE(program.info.texture_descriptors[1].cbuf_offset == 0x600);
    REQUIRE(program.info.texture_descriptors[1].type == type);
}

TEST_CASE("Ampere uniform bindless TEX preserves packed handles and split RGBA results",
          "[video_core][ampere][bindless_tex]") {
    const auto lowered = LowerToMaxwell(sample);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    REQUIRE(lowered.source_size == sizeof(sample));
    REQUIRE(lowered.instructions.size() == 9);
    REQUIRE(lowered.source_instruction_offsets == std::vector<u32>{0, 1, 8});
    REQUIRE(lowered.instructions[1] == Copy(250, 2));
    REQUIRE(lowered.instructions[2] == Copy(251, 3));
    REQUIRE(lowered.instructions[3] ==
            (0xde80000000070000ULL | 250ULL | (250ULL << 8) | (220ULL << 20) |
             (2ULL << 28) | (15ULL << 31) | (1ULL << 37) | (1ULL << 49) | (7ULL << 51)));
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(lowered.instructions[4 + i] == Copy(i, 250 + i));
    }
}

TEST_CASE("Ampere uniform bindless TEX preserves two captured independently merged handles",
          "[video_core][ampere][bindless_tex]") {
    const std::array<Instruction, 13> code{{
        {{0x00067ab9, 0x00815a00, 0x00000800, 0x000fe400}},
        {{0x00047ab9, 0x00805a00, 0x00000800, 0x000fe400}},
        {{0x00077ab9, 0x00805c00, 0x00000800, 0x000fe400}},
        {{0x06047292, 0x00000004, 0x0f8efc3f, 0x000fe400}},
        {{0x06067292, 0x00000007, 0x0f8efc3f, 0x000fc400}},
        {{0x3f057899, 0x00000014, 0x08011604, 0x000fe400}},
        {{0x3f077899, 0x00000014, 0x08011606, 0x000fe200}},
        {{0x00007308, 0x00000000, 0x00001000, 0x001e6400}},
        {{0x00047220, 0x00000001, 0x0041c000, 0x042fe200}},
        {{0x00057220, 0x00000005, 0x0041c000, 0x004fca00}},
        {{0x04087f60, 0x30000605, 0x081e03ff, 0x100f4200}},
        {{0x04067f60, 0x30000405, 0x081e01ff, 0x000f4200}},
        sample[2],
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    for (const u32 native : {10U, 11U}) {
        const u32 start = lowered.source_instruction_offsets[native];
        const u64 texture = lowered.instructions[start + 2];
        REQUIRE(((texture >> 20) & 255) == (native == 10U ? 222U : 220U));
        REQUIRE(((texture >> 31) & 15) == (native == 10U ? 3U : 1U));
        REQUIRE(((texture >> 49) & 1) == 0);
    }
    TextureEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    const u32 last = static_cast<u32>(lowered.instructions.size() - 1);
    Shader::Maxwell::Translate(env, &block, 0x50, 0x50 + last * 8 + ((last + 1) / 3) * 8);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host_info);
    REQUIRE(program.info.texture_descriptors.size() == 2);
    for (u32 i = 0; i < 2; ++i) {
        const auto& descriptor = program.info.texture_descriptors[i];
        REQUIRE(descriptor.type == Shader::TextureType::Color2D);
        REQUIRE(descriptor.has_secondary);
        REQUIRE(descriptor.cbuf_index == 2);
        REQUIRE(descriptor.cbuf_offset == (i == 0 ? 0x168U : 0x170U));
        REQUIRE(descriptor.secondary_cbuf_index == 2);
        REQUIRE(descriptor.secondary_cbuf_offset == 0x568);
        REQUIRE(descriptor.shift_left == 0);
        REQUIRE(descriptor.secondary_shift_left == 0);
        REQUIRE(descriptor.count == 1);
    }
    for (const u32 native : {5U, 6U}) {
        auto clobbered = code;
        clobbered[native] = {{0x7882U | (code[native].Bits(16, 8) << 16), 0, 0, 0}};
        const auto rejected = LowerToMaxwell(clobbered);
        REQUIRE_FALSE(static_cast<bool>(rejected));
        REQUIRE(rejected.error_offset == (native == 5 ? 11U : 10U) * 16);
        REQUIRE(rejected.instructions.empty());
    }
}

TEST_CASE("Ampere uniform bindless TEX admits only forward branches outside its protected interval",
          "[video_core][ampere][bindless_tex]") {
    for (const u32 predicate : {0U, 7U, 8U}) {
        const std::array<Instruction, 5> later{{
            sample[0], sample[1], Branch(16, predicate), {{0x00047882, 0, 0, 0}}, sample[2],
        }};
        const auto lowered = LowerToMaxwell(later);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.source_size == sizeof(later));
        REQUIRE(lowered.source_instruction_offsets.size() == later.size());
        const std::array<Instruction, 4> enter_shift{{Branch(0, predicate), sample[0], sample[1], sample[2]}};
        REQUIRE(static_cast<bool>(LowerToMaxwell(enter_shift)));
        const std::array<Instruction, 4> skip_texture{{Branch(32, predicate), sample[0], sample[1], sample[2]}};
        REQUIRE(static_cast<bool>(LowerToMaxwell(skip_texture)));
    }
    auto extra_condition = Branch(16, 0);
    extra_condition.words[2] = 0x01000000;
    const std::array<Instruction, 5> conditional{{sample[0], sample[1], extra_condition,
                                                {{0x00047882, 0, 0, 0}}, sample[2]}};
    REQUIRE(static_cast<bool>(LowerToMaxwell(conditional)));
    for (const s32 relative : {16, 32}) {
        const std::array<Instruction, 5> enter_interval{{
            Branch(relative), sample[0], {{0x09047220, 4, 0x0041c000, 0}}, sample[1], sample[2],
        }};
        const auto rejected = LowerToMaxwell(enter_interval);
        REQUIRE_FALSE(static_cast<bool>(rejected));
        REQUIRE(rejected.error_offset == 48);
    }
    const std::array<Instruction, 5> loop_clobber{{
        sample[0], sample[1], {{0x00047882, 0, 0, 0}}, Branch(-64), sample[2],
    }};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(loop_clobber)));
    for (const s32 relative : {-48, -16, 1, 1024}) {
        const std::array<Instruction, 4> invalid{{sample[0], sample[1], Branch(relative), sample[2]}};
        const auto rejected = LowerToMaxwell(invalid);
        REQUIRE_FALSE(static_cast<bool>(rejected));
        REQUIRE(rejected.error_offset == 16);
        REQUIRE(rejected.instructions.empty());
    }
    for (const u32 bit : {16U, 84U, 96U}) {
        auto unknown_branch = Branch(0);
        unknown_branch.words[bit / 32] ^= 1U << (bit % 32);
        const std::array<Instruction, 4> invalid{{sample[0], sample[1], unknown_branch, sample[2]}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    const std::array<Instruction, 4> interrupted{{sample[0], Branch(0), sample[1], sample[2]}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(interrupted)));
}

TEST_CASE("Ampere uniform bindless TEX preserves predicates and accepts verified even handles",
          "[video_core][ampere][bindless_tex]") {
    for (u32 handle = 0; handle < 16; handle += 2) {
        for (const u32 predicate : {0U, 7U, 10U}) {
            auto code = sample;
            code[0].words[0] = 0x3f007899U | ((handle + 1) << 16);
            code[0].words[2] = 0x08011600U | handle;
            code[1].words[0] = (code[1].words[0] & ~0xf000U) | (predicate << 12);
            code[1].words[1] = 0x30000003U | (handle << 8);
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(lowered));
            REQUIRE(((lowered.instructions[3] >> 20) & 255) == 216 + handle);
            for (u32 i = 1; i < 8; ++i) {
                REQUIRE(((lowered.instructions[i] >> 16) & 15) == predicate);
            }
        }
    }
}

TEST_CASE("Ampere uniform bindless TEX preserves captured implicit LOD operands",
          "[video_core][ampere][bindless_tex]") {
    const std::array<Instruction, 6> code{{
        sample[0],
        {{0x00097308, 0x00000001, 0x00001000, 0x001e6400}},
        {{0x09047220, 0x00000004, 0x0041c000, 0x042fe200}},
        {{0x09057220, 0x00000000, 0x0041c000, 0x004fcc00}},
        {{0x04067f60, 0x30000405, 0x0c1e0f04, 0x100f4200}},
        sample[2],
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u32 start = lowered.source_instruction_offsets[4];
    REQUIRE(lowered.instructions[start] == Copy(250, 4));
    REQUIRE(lowered.instructions[start + 1] == Copy(251, 5));
    REQUIRE(((lowered.instructions[start + 2] >> 37) & 7) == 0);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(lowered.instructions[start + 3 + i] == Copy(i < 2 ? 6 + i : 2 + i, 250 + i));
    }
}

TEST_CASE("Ampere uniform bindless TEX stops provenance at uniform writes and unknown opcodes",
          "[video_core][ampere][bindless_tex]") {
    for (const u32 target : {4U, 5U, 7U}) {
        for (const Instruction clobber : {
                 Instruction{{0x00007ab9U | (target << 16), 0x00805a00, 0x800, 0}},
                 Instruction{{0x00007882U | (target << 16), 0, 0, 0}},
                 Instruction{{0x00007c82U | (target << 16), 63, 0x08000000, 0}},
                 Instruction{{0x3f007899U | (target << 16), 19, 0x08011604, 0}},
             }) {
            const std::array<Instruction, 5> code{{
                sample[0], clobber, {{0x00097308, 1, 0x1000, 0}}, sample[1], sample[2],
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE_FALSE(static_cast<bool>(lowered));
            REQUIRE(lowered.error_offset == 48);
            REQUIRE(lowered.instructions.empty());
        }
    }
    for (const Instruction barrier : {
             Instruction{{0x00047ab9, 0x00805a00, 0xa00, 0}},
             Instruction{{0x00007802, 0, 0xf00, 0}},
             Instruction{{0x0000ffff, 0, 0, 0}},
         }) {
        const std::array<Instruction, 4> code{{sample[0], barrier, sample[1], sample[2]}};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 32);
        REQUIRE(lowered.instructions.empty());
    }
}

TEST_CASE("Ampere uniform bindless TEX preserves scalar and pair result masks",
          "[video_core][ampere][bindless_tex]") {
    for (const Instruction texture : {
             Instruction{{0x03037f60, 0x30000400, 0x0c1e01ff, 0x10014200}},
             Instruction{{0x04057f60, 0x30000405, 0x0c1e01ff, 0x10014200}},
         }) {
        const std::array<Instruction, 3> code{{sample[0], texture, sample[2]}};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 6);
        REQUIRE(lowered.instructions[1] == Copy(250, texture.Bits(24, 8)));
        REQUIRE(lowered.instructions[2] == Copy(251, texture.Bits(32, 8)));
        REQUIRE(((lowered.instructions[3] >> 31) & 15) == 1);
        REQUIRE(lowered.instructions[4] == Copy(texture.Bits(16, 8), 250));
    }
    for (const u32 mask : {1U, 3U, 15U}) {
        auto code = sample;
        code[1].words[2] = 0x0c9e0000U | (mask << 8) | (mask == 15 ? 2U : 255U);
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u32 count = static_cast<u32>(std::popcount(mask));
        REQUIRE(lowered.instructions.size() == 5 + count);
        REQUIRE(((lowered.instructions[3] >> 31) & 15) == mask);
        for (u32 i = 0; i < count; ++i) REQUIRE(lowered.instructions[4 + i] == Copy(i, 250 + i));
    }
    for (const u32 mask : {0U, 2U, 7U, 14U}) {
        auto code = sample;
        code[1].words[2] = 0x0c9e00ffU | (mask << 8);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 mask : {1U, 3U}) {
        for (const u32 target : {214U, 215U, 216U}) {
            auto code = sample;
            code[1].words[0] = (code[1].words[0] & ~0x00ff0000U) | (target << 16);
            code[1].words[2] = 0x0c9e00ffU | (mask << 8);
            REQUIRE(static_cast<bool>(LowerToMaxwell(code)) ==
                    (target < 216 && (mask == 1 || target == 214)));
        }
        auto invalid_second = sample;
        invalid_second[1].words[2] = 0x0c9e0002U | (mask << 8);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid_second)));
    }
}

TEST_CASE("Ampere uniform bindless TEX bounds the provenance scan",
          "[video_core][ampere][bindless_tex]") {
    for (const u32 count : {31U, 32U}) {
        std::vector<Instruction> code{sample[0]};
        for (u32 i = 0; i < count; ++i) code.push_back({{0x09047220, 4, 0x0041c000, 0}});
        code.push_back(sample[1]);
        code.push_back(sample[2]);
        REQUIRE(static_cast<bool>(LowerToMaxwell(code)) == (count == 31));
    }
}

TEST_CASE("Ampere uniform bindless TEX allows later fragment discard but does not scan past it",
          "[video_core][ampere][bindless_tex]") {
    constexpr Instruction discard{{0x0000895b, 0, 0x03800000, 0}};
    const std::array<Instruction, 4> code{{sample[0], sample[1], discard, sample[2]}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 10);
    REQUIRE(lowered.instructions[8] == 0xe33000000008000fULL);
    const std::array<Instruction, 4> interrupted{{sample[0], discard, sample[1], sample[2]}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(interrupted)));
}

TEST_CASE("Ampere uniform bindless TEX rejects unverified handle pairs and control flow",
          "[video_core][ampere][bindless_tex]") {
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::span{sample}.subspan(1))));
    for (const u32 handle : {1U, 15U, 16U, 28U, 62U, 63U}) {
        auto code = sample;
        code[1].words[1] = 0x30000003U | (handle << 8);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 bit : {12U, 16U, 24U, 32U, 64U, 73U, 76U, 80U, 96U, 100U, 104U}) {
        auto code = sample;
        code[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 opcode : {0x941U, 0x942U, 0x944U, 0x945U, 0x948U,
                             0x949U, 0x950U, 0x958U}) {
        const std::array<Instruction, 4> code{{sample[0], sample[1],
                                             {{0x7000U | opcode, 0, 0x03800000, 0}},
                                             sample[2]}};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 16);
        REQUIRE(lowered.instructions.empty());
    }
    for (const bool forward : {false, true}) {
        const std::array<Instruction, 4> code{{
            {{0x00002947, forward ? 16U : 0xfffffff0U,
              forward ? 0x03800000U : 0x0383ffffU, 0}},
            sample[0], sample[1], sample[2],
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 32);
        REQUIRE(lowered.instructions.empty());
    }
}

TEST_CASE("Ampere uniform bindless TEX does not change bound texture metadata",
          "[video_core][ampere][bindless_tex]") {
    const std::array<Instruction, 4> code{{
        {{0x02007b60, 0x30800003, 0x0c9e0f02, 0x101f4200}},
        sample[0], sample[1], sample[2],
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 2);
}

TEST_CASE("Ampere uniform bindless TEX rejects unsupported texture modes",
          "[video_core][ampere][bindless_tex]") {
    for (const u32 bit : {46U, 54U, 59U, 60U, 61U, 72U, 76U, 77U, 78U, 79U,
                         80U, 81U, 84U, 88U, 89U, 91U, 96U}) {
        auto code = sample;
        code[1].words[bit / 32] ^= 1U << (bit % 32);
        const auto lowered = LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.empty());
    }
}

TEST_CASE("Ampere uniform bindless TEX protects uniform state and output pairs",
          "[video_core][ampere][bindless_tex]") {
    for (const u32 destination : {1U, 2U, 215U, 216U}) {
        auto code = sample;
        code[1].words[0] = (code[1].words[0] & ~0x00ff0000U) | (destination << 16);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 second_destination : {0U, 1U, 215U, 216U}) {
        auto code = sample;
        code[1].words[2] = (code[1].words[2] & ~0xffU) | second_destination;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    for (const u32 source_bit : {24U, 32U}) {
        auto code = sample;
        code[1].words[source_bit / 32] &= ~(255U << (source_bit % 32));
        code[1].words[source_bit / 32] |= 216U << (source_bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }
    auto discarded = sample;
    discarded[1].words[0] |= 0x00ff0000U;
    discarded[1].words[2] |= 0xffU;
    const auto lowered = LowerToMaxwell(discarded);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 5);
    const std::array<Instruction, 4> alias{{{{0x00dc7802, 0, 0xf00, 0}}, sample[0],
                                          sample[1], sample[2]}};
    const auto aliased = LowerToMaxwell(alias);
    REQUIRE_FALSE(static_cast<bool>(aliased));
    REQUIRE(aliased.error_offset == 0);
}
} // namespace
