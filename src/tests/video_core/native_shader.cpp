// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "video_core/texture_cache/format_lookup_table.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/instruction.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/ir_opt/buffer_texture.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace {
namespace IR = Shader::IR;
using Native = Shader::Ampere::Instruction;

class EmptyEnvironment : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::logic_error("unexpected constant folding"); }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

constexpr std::array<Native, 7> VertexTransform{{
    {{0xff007321, 0x000080ff, 0x00000400, 0x000e2200}}, // load two input components
    {{0x00037a02, 0x00c00000, 0x00000f00, 0x000fe400}}, // scale X
    {{0x00027a02, 0x00c00100, 0x00000f00, 0x000fe200}}, // scale Y
    {{0x00007623, 0x00c00200, 0x00010003, 0x001fe200}}, // X * scale X + offset X
    {{0x01017623, 0x00c00300, 0x00010002, 0x000fc400}}, // Y * scale Y + offset Y
    {{0xff007322, 0x00007000, 0x000004ff, 0x000fe200}}, // store position X/Y
    {{0x0000794d, 0x00000000, 0x03800000, 0x000fea00}},
}};

// Evaluate the existing frontend's emitted IR, independently of native bit
// decoding, to check actual attribute/constant/register data flow.
std::array<float, 2> EvaluateVertex(IR::Block& block, std::array<float, 2> input,
                                  std::array<float, 4> constants, s32 vertex_id = 0) {
    std::unordered_map<const IR::Inst*, u32> values;
    std::unordered_map<const IR::Inst*, std::array<u32, 2>> pairs;
    std::array<u32, 256> regs{};
    std::array<float, 2> output{};
    const auto get = [&](const IR::Value& value) {
        if (!value.IsImmediate()) return values.at(value.Inst());
        if (value.Type() == IR::Type::U1) return static_cast<u32>(value.U1());
        return value.Type() == IR::Type::F32 ? std::bit_cast<u32>(value.F32()) : value.U32();
    };
    for (const auto& inst : block.Instructions()) {
        const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
        switch (inst.GetOpcode()) {
        case IR::Opcode::GetRegister:
            values[&inst] = regs[static_cast<u32>(inst.Arg(0).Reg())];
            break;
        case IR::Opcode::SetRegister:
            if (inst.Arg(0).Reg() != IR::Reg::RZ) regs[static_cast<u32>(inst.Arg(0).Reg())] = arg(1);
            break;
        case IR::Opcode::GetAttributeU32:
            REQUIRE(static_cast<u32>(inst.Arg(0).Attribute()) == 0x2fc / 4);
            values[&inst] = static_cast<u32>(vertex_id);
            break;
        case IR::Opcode::ShiftLeftLogical32:
            values[&inst] = arg(0) << arg(1);
            break;
        case IR::Opcode::ShiftRightArithmetic32:
            values[&inst] = static_cast<u32>(static_cast<s32>(arg(0)) >> arg(1));
            break;
        case IR::Opcode::SLessThan:
            values[&inst] = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1));
            break;
        case IR::Opcode::ULessThan:
            values[&inst] = arg(0) < arg(1);
            break;
        case IR::Opcode::SelectU32:
            values[&inst] = arg(0) ? arg(1) : arg(2);
            break;
        case IR::Opcode::SMin32:
            values[&inst] = static_cast<u32>(std::min(static_cast<s32>(arg(0)), static_cast<s32>(arg(1))));
            break;
        case IR::Opcode::SMax32:
            values[&inst] = static_cast<u32>(std::max(static_cast<s32>(arg(0)), static_cast<s32>(arg(1))));
            break;
        case IR::Opcode::INeg32:
            values[&inst] = 0U - arg(0);
            break;
        case IR::Opcode::IAdd32:
            values[&inst] = arg(0) + arg(1);
            break;
        case IR::Opcode::ConvertF32S32:
            values[&inst] = std::bit_cast<u32>(static_cast<float>(static_cast<s32>(arg(0))));
            break;
        case IR::Opcode::GetAttribute: {
            const auto attr = static_cast<u32>(inst.Arg(0).Attribute());
            if (attr == 0x2fc / 4) {
                values[&inst] = static_cast<u32>(vertex_id);
                break;
            }
            REQUIRE(attr >= 0x80 / 4);
            REQUIRE(attr < 0x88 / 4);
            values[&inst] = std::bit_cast<u32>(input[attr - 0x80 / 4]);
            break;
        }
        case IR::Opcode::GetCbufU32x2:
            REQUIRE(arg(0) == 3);
            REQUIRE(arg(1) % 4 == 0);
            REQUIRE(arg(1) + 8 <= sizeof(constants));
            pairs[&inst] = {std::bit_cast<u32>(constants[arg(1) / 4]), std::bit_cast<u32>(constants[arg(1) / 4 + 1])};
            break;
        case IR::Opcode::CompositeExtractU32x2:
            values[&inst] = pairs.at(inst.Arg(0).Inst()).at(arg(1));
            break;
        case IR::Opcode::GetCbufF32:
        case IR::Opcode::GetCbufU32:
            REQUIRE(arg(0) == 3);
            REQUIRE(arg(1) < 16);
            values[&inst] = std::bit_cast<u32>(constants[arg(1) / 4]);
            break;
        case IR::Opcode::BitCastF32U32:
        case IR::Opcode::BitCastU32F32:
            values[&inst] = arg(0);
            break;
        case IR::Opcode::LogicalAnd:
            values[&inst] = arg(0) && arg(1);
            break;
        case IR::Opcode::LogicalOr:
            values[&inst] = arg(0) || arg(1);
            break;
        case IR::Opcode::LogicalNot:
            values[&inst] = !arg(0);
            break;
        case IR::Opcode::FPIsNan32:
            values[&inst] = std::isnan(std::bit_cast<float>(arg(0)));
            break;
        case IR::Opcode::FPOrdGreaterThanEqual32:
            values[&inst] = std::bit_cast<float>(arg(0)) >= std::bit_cast<float>(arg(1));
            break;
        case IR::Opcode::FPRoundEven32:
            values[&inst] = std::bit_cast<u32>(std::nearbyint(std::bit_cast<float>(arg(0))));
            break;
        case IR::Opcode::FPFloor32:
            values[&inst] = std::bit_cast<u32>(std::floor(std::bit_cast<float>(arg(0))));
            break;
        case IR::Opcode::FPTrunc32:
            values[&inst] = std::bit_cast<u32>(std::trunc(std::bit_cast<float>(arg(0))));
            break;
        case IR::Opcode::FPClamp32:
            values[&inst] = std::bit_cast<u32>(std::clamp(std::bit_cast<float>(arg(0)),
                std::bit_cast<float>(arg(1)), std::bit_cast<float>(arg(2))));
            break;
        case IR::Opcode::ConvertU32F32:
            values[&inst] = static_cast<u32>(std::bit_cast<float>(arg(0)));
            break;
        case IR::Opcode::ConvertF32U32:
            values[&inst] = std::bit_cast<u32>(static_cast<float>(arg(0)));
            break;
        case IR::Opcode::BitFieldUExtract:
            values[&inst] = (arg(0) >> arg(1)) & ((1U << arg(2)) - 1);
            break;
        case IR::Opcode::BitwiseOr32:
            values[&inst] = arg(0) | arg(1);
            break;
        case IR::Opcode::FPOrdLessThan32:
            values[&inst] = std::bit_cast<float>(arg(0)) < std::bit_cast<float>(arg(1));
            break;
        case IR::Opcode::BitwiseXor32:
            values[&inst] = arg(0) ^ arg(1);
            break;
        case IR::Opcode::BitwiseNot32:
            values[&inst] = ~arg(0);
            break;
        case IR::Opcode::BitwiseAnd32:
            values[&inst] = arg(0) & arg(1);
            break;
        case IR::Opcode::FPAdd32:
            values[&inst] = std::bit_cast<u32>(std::bit_cast<float>(arg(0)) + std::bit_cast<float>(arg(1)));
            break;
        case IR::Opcode::FPMul32:
            values[&inst] = std::bit_cast<u32>(std::bit_cast<float>(arg(0)) * std::bit_cast<float>(arg(1)));
            break;
        case IR::Opcode::FPMin32:
            values[&inst] = std::bit_cast<u32>(std::fmin(std::bit_cast<float>(arg(0)), std::bit_cast<float>(arg(1))));
            break;
        case IR::Opcode::FPMax32:
            values[&inst] = std::bit_cast<u32>(std::fmax(std::bit_cast<float>(arg(0)), std::bit_cast<float>(arg(1))));
            break;
        case IR::Opcode::SelectF32:
            values[&inst] = arg(0) ? arg(1) : arg(2);
            break;
        case IR::Opcode::FPFma32: {
            const auto control = inst.Flags<IR::FpControl>();
            REQUIRE(control.fmz_mode == IR::FmzMode::FTZ);
            REQUIRE(control.rounding == IR::FpRounding::RN);
            values[&inst] = std::bit_cast<u32>(std::fma(std::bit_cast<float>(arg(0)),
                std::bit_cast<float>(arg(1)), std::bit_cast<float>(arg(2))));
            break;
        }
        case IR::Opcode::SetAttribute: {
            const auto attr = static_cast<u32>(inst.Arg(0).Attribute());
            REQUIRE(attr >= 0x70 / 4);
            REQUIRE(attr < 0x78 / 4);
            output[attr - 0x70 / 4] = std::bit_cast<float>(arg(1));
            break;
        }
        default:
            FAIL("Unexpected IR operation " << IR::NameOf(inst.GetOpcode()));
        }
    }
    return output;
}
} // namespace

TEST_CASE("Native vertex lowering preserves attribute transforms through existing shader IR", "[native-shader]") {
    const auto lowered = Shader::Ampere::LowerToMaxwell(VertexTransform);
    REQUIRE(lowered);
    REQUIRE(lowered.source_size == VertexTransform.size() * 16);
    REQUIRE(lowered.instructions.size() >= VertexTransform.size());
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const u64 word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::FFMA_rc: visitor.FFMA_rc(word); break;
        case Opcode::FFMA_reg: visitor.FFMA_reg(word); break;
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected lowered vertex instruction");
        }
    }
    REQUIRE(EvaluateVertex(block, {2, 4}, {3, 7, 5, 11}) == std::array<float, 2>{11, 39});
    REQUIRE(EvaluateVertex(block, {-4, 0.5f}, {-2, 4, 1, -3}) == std::array<float, 2>{9, -1});
}

TEST_CASE("Native lowering rejects unsupported forms without a partial executable", "[native-shader]") {
    auto code = VertexTransform;
    code[3].words[2] |= 1U << 12; // DNZ has no accepted lowering yet.
    auto result = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE_FALSE(result);
    REQUIRE(result.error_offset == 3 * 16);
    REQUIRE(result.instructions.empty());
    code = VertexTransform;
    code[1].words[2] &= ~(15U << 8); // Partial quad-lane mask.
    result = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE_FALSE(result);
    REQUIRE(result.instructions.empty());
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell({}));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::span{VertexTransform}.first(6)));
}

TEST_CASE("Native instruction predicates survive lowering", "[native-shader]") {
    auto code = VertexTransform;
    code[3].words[0] = (code[3].words[0] & ~0xf000U) | 0x9000U;
    const auto result = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(result);
    const auto pred = Shader::Maxwell::Instruction{result.instructions[result.source_instruction_offsets[3]]}.Pred();
    REQUIRE(pred.index == 1);
    REQUIRE(pred.negated);
}

// These private input fixtures are read from the investigation directory, not
// embedded or distributed with the emulator's source tree.
TEST_CASE("Captured native shader pair compiles through existing control flow and shader IR", "[.native-shader-captured]") {
    const char* directory = std::getenv("EDEN_NATIVE_SHADER_FIXTURES");
    REQUIRE(directory != nullptr);
    class FixtureEnvironment final : public EmptyEnvironment {
    public:
        FixtureEnvironment(const std::vector<u64>& code_, std::span<const u8> header, Shader::Stage stage_)
            : code{code_} {
            std::memcpy(&sph, header.data(), sizeof(sph));
            stage = stage_;
            start_address = 0;
            is_proprietary_driver = true;
        }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        u32 TranslateProgramAddress(u32 address) const override {
            const auto translated = Shader::Ampere::NativeProgramAddress(code, address);
            if (!translated) throw std::logic_error("invalid private native branch target " + std::to_string(address));
            return *translated;
        }
        u32 LocalMemorySize() const override { return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size; }
        u32 TextureBoundBuffer() const override { return 2; }
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            if (bank == 1) {
                REQUIRE(offset % 4 == 0);
                return cbuf1.at(offset / 4);
            }
            REQUIRE(bank == 2);
            return 0; // Synthetic texture handle; only compilation is tested here.
        }
        Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
        const std::vector<u64>& code;
        std::vector<u32> cbuf1;
    };
    for (const int stage : {1, 5}) {
        const auto path = std::filesystem::path{directory} / ("native-graphics-shader-" + std::to_string(stage) + ".bin");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        REQUIRE(file.good());
        const auto size = static_cast<size_t>(file.tellg());
        REQUIRE(size >= 0x90);
        std::vector<u8> bytes(size);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        std::vector<Native> input((size - 0x80) / 16);
        std::memcpy(input.data(), bytes.data() + 0x80, input.size() * 16);
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        INFO("stage=" << stage << " offset=" << lowered.error_offset << " error=" << lowered.error);
        REQUIRE(lowered);
        Shader::Ampere::ProgramCaptureState capture;
        u32 captured_size{};
        for (u32 i = 0; i < input.size(); ++i) {
            const auto status = capture.Observe(input[i], i);
            REQUIRE(status != Shader::Ampere::ProgramCaptureState::Result::InvalidTarget);
            if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                captured_size = (i + 1) * sizeof(Native);
                break;
            }
        }
        REQUIRE(captured_size == lowered.source_size);
        std::vector<u64> code(0x50 / 8);
        for (const u64 word : lowered.instructions) {
            if ((code.size() * 8) % 32 == 0) code.push_back(0);
            code.push_back(word);
        }
        code.push_back(0);
        Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
        FixtureEnvironment env{code, std::span{bytes}.first(0x50), stage == 1 ? Shader::Stage::VertexB : Shader::Stage::Fragment};
        auto constants_path = std::filesystem::path{directory} / ("cbuf1-" + std::to_string(stage) + ".bin");
        if (!std::filesystem::exists(constants_path)) constants_path = std::filesystem::path{directory} / "cbuf1.bin";
        std::ifstream constants(constants_path, std::ios::binary | std::ios::ate);
        if (constants) {
            env.cbuf1.resize(static_cast<size_t>(constants.tellg()) / 4);
            constants.seekg(0);
            constants.read(reinterpret_cast<char*>(env.cbuf1.data()), env.cbuf1.size() * 4);
        }
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::ObjectPool<IR::Inst> inst_pool;
        Shader::ObjectPool<IR::Block> block_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
        Shader::HostTranslateInfo host;
        host.support_int64 = true;
        host.support_float64 = true;
        host.support_float16 = true;
        const auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
        REQUIRE(program.stage == env.ShaderStage());
        REQUIRE_FALSE(program.blocks.empty());
        REQUIRE_FALSE(program.post_order_blocks.empty());
    }
}

TEST_CASE("Native scalar texture sampling preserves coordinates and split outputs", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0x04007b60, 0x30800805, 0x041e0f02, 0x100fc200}},
        VertexTransform.back(),
    }};
    const u32 binding = GENERATE(0U, 2U, 17U);
    input[0].words[1] = (input[0].words[1] & ~(31U << 22)) | (binding << 22);
    const bool predicated = GENERATE(false, true);
    if (predicated) input[0].words[0] = (input[0].words[0] & ~0xf000U) | 0x3000;
    const bool explicit_lod = GENERATE(false, true);
    if (explicit_lod) input[0].words[2] |= 1U << 23;
    const auto result = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(result);
    REQUIRE(result.required_texture_buffer == binding);
    REQUIRE(Shader::Maxwell::Instruction{result.instructions.front()}.Pred().index == (predicated ? 3U : 7U));
    REQUIRE_FALSE(Shader::Maxwell::Instruction{result.instructions.front()}.Pred().negated);
    REQUIRE(Shader::Maxwell::Decode(result.instructions.front()) == Shader::Maxwell::Opcode::TEXS);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TEXS(result.instructions.front());
    std::vector<u32> reads;
    std::vector<u32> writes;
    u32 samples = 0;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister) reads.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::SetRegister) writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == (explicit_lod ? IR::Opcode::BoundImageSampleExplicitLod : IR::Opcode::BoundImageSampleImplicitLod)) {
            ++samples;
            if (explicit_lod) REQUIRE(inst.Arg(2).F32() == 0.0f);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructF32x2);
            for (size_t component = 0; component < 2; ++component) {
                const auto* cast = coords->Arg(component).Inst();
                REQUIRE(cast->GetOpcode() == IR::Opcode::BitCastF32U32);
                const auto* reg = cast->Arg(0).Inst();
                REQUIRE(reg->GetOpcode() == IR::Opcode::GetRegister);
                REQUIRE(static_cast<u32>(reg->Arg(0).Reg()) == 4 + component);
            }
            REQUIRE(inst.Arg(0).U32() == 0x20);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type == Shader::TextureType::Color2D);
        }
    }
    REQUIRE(samples == 1);
    std::ranges::sort(reads);
    REQUIRE(reads == std::vector<u32>{4, 5});
    REQUIRE(writes == std::vector<u32>{0, 1, 2, 3});
    // Reject unsupported LOD modes and sparse results.
    for (u32 bit : {89U, 81U}) {
        auto unsupported = input;
        unsupported[0].words[bit / 32] ^= 1U << (bit % 32);
        const auto rejected = Shader::Ampere::LowerToMaxwell(unsupported);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.instructions.empty());
    }
    auto unsupported = input;
    unsupported[0].words[1] = (unsupported[0].words[1] & ~(31U << 22)) | (18U << 22);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
}

TEST_CASE("Native texel fetch keeps integer coordinates and RGB outputs", "[native-shader]") {
    std::array<Native, 3> input{{
        {{0x00027305, 0x00000004, 0x0021f100, 0x001ff000}},
        {{0x02007b66, 0x30800a05, 0x049e0702, 0x101fc200}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 2);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[0]) == Shader::Maxwell::Opcode::F2I_reg);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[1]) == Shader::Maxwell::Opcode::TLDS);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.F2I_reg(lowered.instructions[0]);
    visitor.TLDS(lowered.instructions[1]);
    std::vector<u32> writes;
    u32 fetches = 0;
    bool truncates = false;
    bool handles_nan = false;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister) writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::FPTrunc32) {
            truncates = true;
            REQUIRE(inst.Flags<IR::FpControl>().fmz_mode == IR::FmzMode::FTZ);
        }
        if (inst.GetOpcode() == IR::Opcode::FPIsNan32) handles_nan = true;
        if (inst.GetOpcode() == IR::Opcode::BoundImageFetch) {
            ++fetches;
            REQUIRE(inst.Arg(0).U32() == 0x28);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type == Shader::TextureType::Color2D);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructU32x2);
            REQUIRE(coords->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{2});
            REQUIRE(coords->Arg(1).Inst()->Arg(0).Reg() == IR::Reg{5});
            REQUIRE(inst.Arg(3).U32() == 0);
        }
    }
    REQUIRE(fetches == 1);
    REQUIRE(truncates);
    REQUIRE(handles_nan);
    REQUIRE(writes == std::vector<u32>{2, 0, 1, 2});
    auto unsupported = input;
    unsupported[0].words[2] ^= 1U << 13; // NTZ is required by this conversion lowering.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
    unsupported = input;
    unsupported[1].words[2] ^= 1U << 14; // multisample fetch is not implemented here.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
}

TEST_CASE("Native texture component masks preserve sampled components", "[native-shader]") {
    for (const u32 mask : {1U, 2U, 4U, 8U, 3U, 9U, 10U, 12U, 7U, 11U, 13U, 14U, 15U}) {
        const bool split = std::popcount(mask) > 2;
        std::array<Native, 2> input{{
            {{0x04007b60, 0x30800805, 0x041e0f02, 0x100fc200}}, VertexTransform.back(),
        }};
        input[0].words[2] = (input[0].words[2] & ~0xfffU) | (split ? 6U : 255U) | (mask << 8);
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        REQUIRE(lowered);
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        EmptyEnvironment env;
        Shader::Maxwell::TranslatorVisitor visitor{env, block};
        visitor.TEXS(lowered.instructions.front());
        std::vector<u32> extracted;
        std::vector<u32> writes;
        for (const auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4) extracted.push_back(inst.Arg(1).U32());
            if (inst.GetOpcode() == IR::Opcode::SetRegister) writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        }
        std::vector<u32> expected;
        for (u32 component = 0; component < 4; ++component) if (mask & (1U << component)) expected.push_back(component);
        REQUIRE(extracted == expected);
        REQUIRE(writes.size() == expected.size());
        for (u32 i = 0; i < writes.size(); ++i) REQUIRE(writes[i] == (i < 2 ? i : i + 4));
    }
}

TEST_CASE("Native half packing and arithmetic preserve lane order numerically", "[native-shader]") {
    const std::array<Native, 15> input{{
        {{0x0102723e, 0x00000000, 0x000000ff, 0}}, // PACK_AB R2, R1, R0
        {{0x02027232, 0x00000002, 0x00000000, 0}}, // HMUL2 R2, R2, R2
        {{0xff037230, 0x20000002, 0x00004100, 0}}, // extract low half to F32 R3
        {{0xff047230, 0x30000002, 0x00004100, 0}}, // extract high half to F32 R4
        {{0xff05723e, 0x00000000, 0x00004002, 0}}, // MERGE_C R5, R0, R2
        {{0x02067230, 0x20000002, 0x00000800, 0}}, // add two low halves: (2,2)
        {{0x02077231, 0x20000002, 0x00040802, 0}}, // fused 1*1+1: (2,2)
        {{0x02087240, 0x30000002, 0x03800800, 0}}, // min of low/high: (1,1)
        {{0x02097840, 0x3c003c00, 0x07800c00, 0}}, // max of high/one: (4,4)
        {{0x000a7308, 0x00000008, 0x00001200, 0}}, // reciprocal of half one
        {{0x020b7816, 0x00005410, 0x00000007, 0}}, // concatenate low halves
        {{0xff0c7230, 0xa0000008, 0x00004100, 0}}, // extract negated low half
        {{0x020d7230, 0x30000002, 0x00000f00, 0}}, // -abs(high) + high: (0,0)
        {{0x020e7431, 0x3c003c00, 0x00400802, 0}}, // (1,1) * (1,-4) + (1,1)
        VertexTransform.back(),
    }};
    auto code = input;
    const u32 selector = GENERATE(0x5410U, 0x5432U, 0x7610U, 0x7632U);
    code[10].words[1] = selector;
    if (selector == 0x7610 || selector == 0x7632) code[10].words[2] = 2;
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const u64 word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::F2F_reg: visitor.F2F_reg(word); break;
        case Opcode::IADD_reg: visitor.IADD_reg(word); break;
        case Opcode::ISCADD_reg: visitor.ISCADD_reg(word); break;
        case Opcode::HMUL2_reg: visitor.HMUL2_reg(word); break;
        case Opcode::HADD2_reg: visitor.HADD2_reg(word); break;
        case Opcode::HFMA2_reg: visitor.HFMA2_reg(word); break;
        case Opcode::FMNMX_reg: visitor.FMNMX_reg(word); break;
        case Opcode::MUFU: visitor.MUFU(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::SHR_imm: visitor.SHR_imm(word); break;
        case Opcode::LOP32I: visitor.LOP32I(word); break;
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected canonical half instruction");
        }
    }
    // Independent values with exact binary16 representations exercise packing,
    // multiplication and extraction through the actual emitted IR data flow.
    const std::array<std::pair<u16, float>, 5> halves{{{u16{0x4200}, 3.0f}, {u16{0}, 0.0f}, {u16{0x3c00}, 1.0f}, {u16{0x4000}, 2.0f}, {u16{0x4400}, 4.0f}}};
    const auto from_half = [&](u16 bits) {
        for (const auto& [encoded, value] : halves) if (encoded == (bits & 0x7fff)) return (bits & 0x8000) ? -value : value;
        throw std::logic_error("unexpected half value");
    };
    const auto to_half = [&](float value) {
        for (const auto& [encoded, reference] : halves) if (std::abs(value) == reference) return static_cast<u16>(encoded | (std::signbit(value) ? 0x8000 : 0));
        throw std::logic_error("unexpected float value");
    };
    std::array<u32, 256> regs{};
    regs[0] = std::bit_cast<u32>(1.0f);
    regs[1] = std::bit_cast<u32>(2.0f);
    std::unordered_map<const IR::Inst*, u64> values;
    const auto get = [&](const IR::Value& value) -> u64 {
        if (!value.IsImmediate()) return values.at(value.Inst());
        if (value.Type() == IR::Type::U1) return value.U1();
        if (value.Type() == IR::Type::F32) return std::bit_cast<u32>(value.F32());
        if (value.Type() == IR::Type::U16 || value.Type() == IR::Type::F16) return value.U16();
        return value.U32();
    };
    for (const auto& inst : block.Instructions()) {
        const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
        switch (inst.GetOpcode()) {
        case IR::Opcode::GetRegister: values[&inst] = regs[static_cast<u32>(inst.Arg(0).Reg())]; break;
        case IR::Opcode::SetRegister: regs[static_cast<u32>(inst.Arg(0).Reg())] = static_cast<u32>(arg(1)); break;
        case IR::Opcode::BitCastF32U32:
        case IR::Opcode::BitCastU32F32: values[&inst] = arg(0); break;
        case IR::Opcode::ConvertF16F32: values[&inst] = to_half(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
        case IR::Opcode::ConvertF32F16: values[&inst] = std::bit_cast<u32>(from_half(static_cast<u16>(arg(0)))); break;
        case IR::Opcode::CompositeConstructF16x2: values[&inst] = arg(0) | (arg(1) << 32); break;
        case IR::Opcode::CompositeExtractF16x2: values[&inst] = (arg(0) >> (arg(1) * 32)) & 0xffff; break;
        case IR::Opcode::PackFloat2x16: values[&inst] = (arg(0) & 0xffff) | ((arg(0) >> 32) << 16); break;
        case IR::Opcode::UnpackFloat2x16: values[&inst] = (arg(0) & 0xffff) | ((arg(0) >> 16) << 32); break;
        case IR::Opcode::FPMul16: values[&inst] = to_half(from_half(static_cast<u16>(arg(0))) * from_half(static_cast<u16>(arg(1)))); break;
        case IR::Opcode::FPAdd16: values[&inst] = to_half(from_half(static_cast<u16>(arg(0))) + from_half(static_cast<u16>(arg(1)))); break;
        case IR::Opcode::FPFma16: values[&inst] = to_half(std::fma(from_half(static_cast<u16>(arg(0))), from_half(static_cast<u16>(arg(1))), from_half(static_cast<u16>(arg(2))))); break;
        case IR::Opcode::FPNeg16: values[&inst] = arg(0) ^ 0x8000; break;
        case IR::Opcode::FPAbs16: values[&inst] = arg(0) & 0x7fff; break;
        case IR::Opcode::FPAdd32: values[&inst] = std::bit_cast<u32>(std::bit_cast<float>(static_cast<u32>(arg(0))) + std::bit_cast<float>(static_cast<u32>(arg(1)))); break;
        case IR::Opcode::FPMin32: values[&inst] = std::bit_cast<u32>(std::fmin(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
        case IR::Opcode::FPMax32: values[&inst] = std::bit_cast<u32>(std::fmax(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
        case IR::Opcode::FPRecip32: values[&inst] = std::bit_cast<u32>(1.0f / std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
        case IR::Opcode::SelectF32: values[&inst] = arg(0) ? arg(1) : arg(2); break;
        case IR::Opcode::BitwiseAnd32: values[&inst] = arg(0) & arg(1); break;
        case IR::Opcode::BitwiseXor32: values[&inst] = arg(0) ^ arg(1); break;

        case IR::Opcode::ShiftRightLogical32: values[&inst] = static_cast<u32>(arg(0)) >> arg(1); break;
        case IR::Opcode::SLessThan: values[&inst] = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1)); break;
        case IR::Opcode::ULessThan: values[&inst] = static_cast<u32>(arg(0)) < static_cast<u32>(arg(1)); break;
        case IR::Opcode::SelectU32: values[&inst] = arg(0) ? arg(1) : arg(2); break;
        case IR::Opcode::IAdd32: values[&inst] = static_cast<u32>(arg(0) + arg(1)); break;
        case IR::Opcode::ShiftLeftLogical32: values[&inst] = static_cast<u32>(arg(0) << arg(1)); break;
        default: FAIL("Unexpected IR operation in half arithmetic");
        }
    }
    REQUIRE(regs[2] == 0x44003c00);
    REQUIRE(regs[5] == 0x3c003c00);
    REQUIRE(regs[6] == 0x40004000U);
    REQUIRE(regs[7] == 0x40004000U);
    REQUIRE(regs[8] == 0x3c003c00U);
    REQUIRE(regs[9] == 0x44004400U);
    REQUIRE(regs[10] == 0x3c00U);
    REQUIRE(regs[11] == (selector == 0x7632 ? 0x44004400U : selector == 0x7610 ? 0x44003c00U : selector == 0x5432 ? 0x40004400U : 0x40003c00U));
    REQUIRE(std::bit_cast<float>(regs[12]) == -1.0f);
    REQUIRE(regs[13] == 0U);
    REQUIRE(regs[14] == 0xc2004000U);

    REQUIRE(std::bit_cast<float>(regs[3]) == 1.0f);
    REQUIRE(std::bit_cast<float>(regs[4]) == 4.0f);
}

TEST_CASE("Live shader overrides require the exact original instructions", "[native-shader]") {
    const auto program = Shader::Ampere::LowerToMaxwell(VertexTransform);
    REQUIRE(program);
    const auto bytes = Shader::Ampere::EncodeProgramOverride(VertexTransform, program);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(Shader::Ampere::ProgramOverrideSourceSize(bytes) == VertexTransform.size() * 16);
    REQUIRE(Shader::Ampere::ProgramOverrideSourceSize(std::span{bytes}.first(12)) == 0);
    auto invalid_source = bytes;
    invalid_source[8] ^= 1;
    REQUIRE(Shader::Ampere::ProgramOverrideSourceSize(invalid_source) == 0);
    const auto decoded = Shader::Ampere::DecodeProgramOverride(bytes, VertexTransform);
    REQUIRE(decoded);
    REQUIRE(decoded.instructions == program.instructions);
    REQUIRE(decoded.source_instruction_offsets == program.source_instruction_offsets);
    auto invalid_map = bytes;
    const u32 invalid_target = static_cast<u32>(program.instructions.size());
    std::memcpy(invalid_map.data() + invalid_map.size() - program.source_instruction_offsets.size() * 4,
                &invalid_target, sizeof(invalid_target));
    const auto invalid_mapping = Shader::Ampere::DecodeProgramOverride(invalid_map, VertexTransform);
    REQUIRE_FALSE(invalid_mapping);
    REQUIRE(invalid_mapping.instructions.empty());

    REQUIRE(decoded.source_size == program.source_size);
    REQUIRE(decoded.required_texture_buffer == program.required_texture_buffer);
    auto different = VertexTransform;
    different[0].words[1] ^= 4;
    REQUIRE(Shader::Ampere::NativeProgramKey(different) != Shader::Ampere::NativeProgramKey(VertexTransform));
    const auto mismatch = Shader::Ampere::DecodeProgramOverride(bytes, different);
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.instructions.empty());
    REQUIRE_FALSE(Shader::Ampere::DecodeProgramOverride(std::span{bytes}.first(12), VertexTransform));
    REQUIRE_FALSE(Shader::Ampere::DecodeProgramOverride(std::span{bytes}.first(bytes.size() - 1), VertexTransform));
    auto wrong_version = bytes;
    wrong_version[4] ^= 1;
    REQUIRE_FALSE(Shader::Ampere::DecodeProgramOverride(wrong_version, VertexTransform));
    auto wrong_binding = bytes;
    const u32 bank = 18;
    std::memcpy(wrong_binding.data() + 16, &bank, sizeof(bank));
    REQUIRE_FALSE(Shader::Ampere::DecodeProgramOverride(wrong_binding, VertexTransform));
}

TEST_CASE("Native integer shader forms preserve signed arithmetic and immediate values", "[native-shader]") {
    // Synthetic vertex program: output (id + arithmetic_shift(id, 1), 4.0).
    const std::array<Native, 7> input{{
        {{0xff007321, 0x0002fcff, 0x00000000, 0x000e2200}},
        {{0xff017819, 0x00000001, 0x00011400, 0x000fe400}},
        {{0x00007824, 0x00000001, 0x078e0201, 0x000fe200}},
        {{0x00007245, 0x00000000, 0x00201400, 0x000fc600}},
        {{0xff017424, 0x40800000, 0x078e00ff, 0x000fe200}},
        {{0xff007322, 0x00007000, 0x000004ff, 0x000fe200}},
        VertexTransform.back(),
    }};
    auto program = input;
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U);
    if (mode == 7) program[2] = Native{{0x00007217, 1, 0x03800200, 0}};
    if (mode == 8) program[2] = Native{{0x00007217, 1, 0x07800200, 0}};
    if (mode == 5) program[2] = Native{{0x00007210, 0x00000001, 0x07ffe1ff, 0}};
    if (mode == 6) program[2] = Native{{0x00007817, 0x0000003f, 0x07800200, 0}};
    if (mode == 4) program[2] = Native{{0x00007817, 0x0000003f, 0x03800200, 0}};
    if (mode == 1) program[2] = Native{{0x00007210, 0x00000001, 0x07ffe0ff, 0x000fe200}};
    if (mode == 3) program[2] = Native{{0x00007210, 0x80000001, 0x07ffe0ff, 0x000fe200}};
    if (mode == 2) program[1] = Native{{0x00017819, 0x00000001, 0x000006ff, 0x000fe200}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(program);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const u64 word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::SHR_imm: visitor.SHR_imm(word); break;
        case Opcode::SHL_imm: visitor.SHL_imm(word); break;
        case Opcode::IMNMX_reg: visitor.IMNMX_reg(word); break;
        case Opcode::IMNMX_imm: visitor.IMNMX_imm(word); break;
        case Opcode::IADD_reg: visitor.IADD_reg(word); break;
        case Opcode::I2F_reg: visitor.I2F_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected lowered integer shader instruction");
        }
    }
    for (const s32 id : {0, 1, 2, -1, -3, 12345}) {
        REQUIRE(EvaluateVertex(block, {}, {}, id) ==
                std::array<float, 2>{static_cast<float>(mode == 2 ? id * 3 : mode == 3 ? id - (id >> 1) : mode == 4 ? std::min<s32>(id, 63) : mode == 5 ? -id + (id >> 1) : mode == 6 ? std::max<s32>(id, 63) : mode == 7 ? std::min<s32>(id, id >> 1) : mode == 8 ? std::max<s32>(id, id >> 1) : id + (id >> 1)), 4.0f});
    }
}

TEST_CASE("Native single-channel texel fetch writes only its selected result", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0x04017b66, 0x30800805, 0x049e01ff, 0x001fc200}},
        VertexTransform.back(),
    }};
    const u32 component = GENERATE(0U, 1U, 2U, 3U);
    input[0].words[2] = (input[0].words[2] & ~0xf00U) | (1U << (8 + component));
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 2);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TLDS(lowered.instructions.front());
    std::vector<u32> writes;
    u32 fetches = 0;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister)
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::BoundImageFetch) {
            ++fetches;
            REQUIRE(inst.Arg(0).U32() == 0x20);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructU32x2);
            REQUIRE(coords->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{4});
            REQUIRE(coords->Arg(1).Inst()->Arg(0).Reg() == IR::Reg{5});
        }
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4)
            REQUIRE(inst.Arg(1).U32() == component);
    }
    REQUIRE(fetches == 1);
    REQUIRE(writes == std::vector<u32>{1});
}

TEST_CASE("Native ALU constants preserve high buffer slots and byte offsets", "[native-shader]") {
    const u32 bank = GENERATE(18U, 24U, 31U);
    const u32 offset = GENERATE(0U, 4U, 0xfffcU);
    std::array<Native, 2> input{{
        {{0x0d117a20, (bank << 22) | (offset << 6), 0x00410000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const u64 word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::FMUL_reg: visitor.FMUL_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected high-slot constant instruction");
        }
    }
    u32 loads{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetCbufU32) {
            REQUIRE(inst.Arg(0).U32() == bank);
            REQUIRE(inst.Arg(1).U32() == offset);
            ++loads;
        }
    }
    REQUIRE(loads == 1);
    input[0].words[1] |= 1U << 6;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
}

TEST_CASE("Native IMAD move loads the actual constant-buffer value", "[native-shader]") {
    const std::array<Native, 3> input{{
        {{0xff007624, 0x00c00100, 0x078e00ff, 0x000fe200}},
        {{0xff007322, 0x00007000, 0x000000ff, 0x000fe200}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::MOV_cbuf);
    visitor.MOV_cbuf(lowered.instructions[0]);
    visitor.AST(lowered.instructions[1]);
    REQUIRE(EvaluateVertex(block, {}, {13, -2.5f, 99, 7}) == std::array<float, 2>{-2.5f, 0.0f});
    auto unsupported = input;
    unsupported[0].words[1] |= 1U << 30;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
}

TEST_CASE("Depth-stencil descriptors ignore the unused blue and alpha component types", "[native-gpu]") {
    using Tegra::Texture::ComponentType;
    using Tegra::Texture::TextureFormat;
    using VideoCore::Surface::PixelFormat;
    for (const auto unused : {ComponentType::UNORM, ComponentType::UINT}) {
        REQUIRE(VideoCommon::PixelFormatFromTextureInfo(TextureFormat::S8Z24,
            ComponentType::UNORM, ComponentType::UINT, unused, unused, false) ==
            PixelFormat::D24_UNORM_S8_UINT);
    }
}

TEST_CASE("Native floating min/max clamps actual shader input", "[native-shader]") {
    const std::array<Native, 5> input{{
        {{0xff007321, 0x000080ff, 0x00000000, 0x000e2200}},
        {{0x00007a09, 0x00c00000, 0x07810000, 0x000fe400}},
        {{0x00007a09, 0x00c00100, 0x03810000, 0x000fe400}},
        {{0xff007322, 0x00007000, 0x000000ff, 0x000fe200}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::FMNMX_reg: visitor.FMNMX_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected native min/max lowering");
        }
    }
    for (const float value : {-20.0f, -1.0f, 0.25f, 2.0f, 100.0f})
        REQUIRE(EvaluateVertex(block, {value, 0}, {-1, 2, 0, 0}) ==
                std::array<float, 2>{std::clamp(value, -1.0f, 2.0f), 0});
}

TEST_CASE("Native boolean float comparison preserves register moves and absolute inputs", "[native-shader]") {
    const std::array<Native, 5> input{{
        {{0xff007321, 0x000080ff, 0x00000000, 0}},
        {{0xff017224, 0x000000ff, 0x078e0000, 0}},
        {{0x01007a0a, 0x00c00000, 0x03811200, 0}},
        {{0xff007322, 0x00007000, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::LOP32I: visitor.LOP32I(word); break;
        case Opcode::FSET_reg: visitor.FSET_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected native boolean float comparison lowering");
        }
    }
    for (const float value : {-3.0f, -1.0f, 0.0f, 2.0f, 4.0f})
        REQUIRE(EvaluateVertex(block, {value, 0}, {2, 0, 0, 0}) ==
                std::array<float, 2>{std::abs(value) < 2.0f ? 1.0f : 0.0f, 0});
}

TEST_CASE("Native unsigned conversion saturates, clears NaNs, and extracts the low byte", "[native-shader]") {
    std::array<Native, 5> input{{
        {{0xff007321, 0x000080ff, 0x00000000, 0}},
        {{0x00017305, 0x00000000, 0x0021f000, 0}},
        {{0x00007306, 0x00000001, 0x00001000, 0}},
        {{0xff007322, 0x00007000, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const bool constant_source = GENERATE(false, true);
    if (constant_source) input[1] = Native{{0x00017b05, 0x00c00000, 0x0021f000, 0}};
    const u32 conversion_mode = GENERATE(0U, 1U, 2U);
    const bool short_round = conversion_mode == 1;
    if (conversion_mode == 2) input[1].words[2] = 0x00207000;
    if (short_round) input[1].words[2] = 0x00212800;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::FSET_reg: visitor.FSET_reg(word); break;
        case Opcode::FMNMX_reg: visitor.FMNMX_reg(word); break;
        case Opcode::LOP_reg: visitor.LOP_reg(word); break;
        case Opcode::F2I_reg: visitor.F2I_reg(word); break;
        case Opcode::I2F_reg: visitor.I2F_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected unsigned conversion lowering");
        }
    }
    const std::array<std::pair<float, float>, 9> cases{{
        {std::numeric_limits<float>::quiet_NaN(), 0.0f},
        {-std::numeric_limits<float>::infinity(), 0.0f}, {-3.0f, 0.0f}, {1.9f, 1.0f},
        {300.75f, 44.0f}, {65535.0f, 255.0f}, {4294967040.0f, 0.0f},
        {4294967296.0f, 255.0f}, {std::numeric_limits<float>::infinity(), 255.0f},
    }};
    for (const auto& [value, expected] : cases) {
        const float rounded = std::isnan(value) ? 0.0f : static_cast<float>(static_cast<u32>(
            std::clamp(std::nearbyint(static_cast<double>(value)), 0.0, 65535.0)) & 255);
        REQUIRE(EvaluateVertex(block, {value, 0}, {value, 0, 0, 0}) == std::array<float, 2>{short_round ? rounded : expected, 0});
    }
}

TEST_CASE("Native depth-comparison sample preserves XY coordinates and reference depth", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0x04017b60, 0x30800806, 0x041e41ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TEXS(lowered.instructions.front());
    u32 samples = 0;
    std::vector<u32> writes;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister)
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::BoundImageSampleDrefImplicitLod) {
            ++samples;
            REQUIRE(inst.Arg(0).U32() == 0x20);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructF32x2);
            for (u32 i = 0; i < 2; ++i)
                REQUIRE(coords->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{4 + i});
            REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{6});
            REQUIRE(inst.Flags<IR::TextureInstInfo>().is_depth == 1);
        }
    }
    REQUIRE(samples == 1);
    REQUIRE(writes == std::vector<u32>{1});
}

TEST_CASE("Native array texel fetch preserves layer and separates XY registers", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0x06007b66, 0xb0800809, 0x009e0f02, 0}},
        VertexTransform.back(),
    }};
    const bool sparse = GENERATE(false, true);
    if (sparse) input[0].words[2] = 0x009e0b23;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.MOV_reg(lowered.instructions[0]);
    visitor.MOV_reg(lowered.instructions[1]);
    visitor.TLDS(lowered.instructions[2]);
    u32 fetches = 0;
    std::vector<u32> writes, components;
    std::vector<std::pair<u32, u32>> copies;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetRegister && static_cast<u32>(inst.Arg(0).Reg()) < 250)
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4)
            components.push_back(inst.Arg(1).U32());
        if (inst.GetOpcode() == IR::Opcode::SetRegister &&
            static_cast<u32>(inst.Arg(0).Reg()) >= 250) {
            REQUIRE(inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister);
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()),
                                static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
        }
        if (inst.GetOpcode() == IR::Opcode::BoundImageFetch) {
            ++fetches;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type == Shader::TextureType::ColorArray2D);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructU32x3);
            REQUIRE(coords->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250});
            REQUIRE(coords->Arg(1).Inst()->Arg(0).Reg() == IR::Reg{251});
            const auto* layer = coords->Arg(2).Inst();
            REQUIRE(layer->GetOpcode() == IR::Opcode::BitFieldUExtract);
            REQUIRE(layer->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{6});
            REQUIRE(layer->Arg(1).U32() == 0);
            REQUIRE(layer->Arg(2).U32() == 16);
        }
    }
    REQUIRE(fetches == 1);
    REQUIRE(writes == (sparse ? std::vector<u32>{0, 1, 35} : std::vector<u32>{0, 1, 2, 3}));
    REQUIRE(components == (sparse ? std::vector<u32>{0, 1, 3} : std::vector<u32>{0, 1, 2, 3}));
    REQUIRE(copies == std::vector<std::pair<u32, u32>>{{250, 7}, {251, 9}});
}

TEST_CASE("Native bitwise LUT forms preserve source bits without changing predicates", "[native-shader]") {
    std::array<Native, 5> input{{
        {{0x00007a02, 0x00c00000, 0x00000f00, 0}},
        {{0x00027a02, 0x00c00200, 0x00000f00, 0}},
        {{0x00007812, 0x80000000, 0x078ef802, 0}},
        {{0xff007322, 0x00007000, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const u32 mode = GENERATE(0U, 1U, 2U, 3U);
    if (mode == 3) input[2] = Native{{0x00007812, 0x80000000, 0x078efe02, 0}};
    if (mode == 2) input[2] = Native{{0x00007812, 0x80000000, 0x078ee202, 0}};
    if (mode == 1) input[2] = Native{{0x00007212, 0x00000002, 0x078efcff, 0}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::LOP_reg: visitor.LOP_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected native bitwise LUT lowering");
        }
    }
    for (const auto& inst : block.Instructions()) REQUIRE(inst.GetOpcode() != IR::Opcode::SetPred);
    for (const float a : {-1.0f, 1.0f}) for (const float c : {-2.0f, 2.0f}) {
        const u32 c_bits = std::bit_cast<u32>(c);
        const u32 a_bits = std::bit_cast<u32>(a);
        const u32 expected = mode == 3 ? a_bits | c_bits | 0x80000000U : mode == 2 ? (a_bits & 0x80000000U) | (c_bits & 0x7fffffffU) :
            a_bits | (mode == 1 ? c_bits : c_bits & 0x80000000U);
        REQUIRE(EvaluateVertex(block, {}, {a, 0, c, 0}) ==
                std::array<float, 2>{std::bit_cast<float>(expected), 0});
    }
}

TEST_CASE("Native texture gathers preserve coordinates component and split result pairs", "[native-shader]") {
    const u32 binding = GENERATE(0U, 5U, 17U);
    const u32 component = GENERATE(0U, 1U, 2U, 3U);
    const std::array<Native, 2> input{{
        {{0x04047b63, 0x30000703U | (binding << 22), 0x041e0f06U | (component << 23), 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == binding);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::TLD4S);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TLD4S(lowered.instructions.front());
    u32 gathers{};
    std::vector<u32> writes, channels;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageGather) {
            ++gathers;
            REQUIRE(inst.Arg(0).U32() == 28);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color2D);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().gather_component.Value() == component);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->Arg(0).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{4});
            REQUIRE(coords->Arg(1).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{3});
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4) channels.push_back(inst.Arg(1).U32());
    }
    REQUIRE(gathers == 1);
    REQUIRE(writes == std::vector<u32>{4, 5, 6, 7});
    REQUIRE(channels == std::vector<u32>{0, 1, 2, 3});
    auto invalid = input;
    invalid[0].words[2] |= 1U << 12;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native array image stores retain a descriptor bank distinct from texture gathers", "[native-shader]") {
    const std::array<Native, 3> input{{
        {{0x04047b63, 0x31400003, 0x041e0f06, 0}},
        {{0x00007b9b, 0x8080480c, 0x00100f00, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 5);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TLD4S(lowered.instructions[0]);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[1]) == Shader::Maxwell::Opcode::MOV_cbuf);
    visitor.MOV_cbuf(lowered.instructions[1]);
    visitor.SUST(lowered.instructions[2]);
    u32 descriptors{}, stores{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetCbufU32) {
            ++descriptors;
            REQUIRE(inst.Arg(0).U32() == 2);
            REQUIRE(inst.Arg(1).U32() == 0x120);
        }
        if (inst.GetOpcode() == IR::Opcode::BindlessImageWrite) {
            ++stores;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorArray2D);
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250});
            REQUIRE(inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::CompositeConstructU32x3);
            const auto* color = inst.Arg(2).Inst();
            for (u32 i = 0; i < 4; ++i) REQUIRE(color->Arg(i).Inst()->Arg(0).Reg() == IR::Reg{12 + i});
        }
    }
    REQUIRE(descriptors == 1);
    REQUIRE(stores == 1);
}

TEST_CASE("Native one-dimensional texel fetch preserves its integer coordinate", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0x04017b66, 0x108008ff, 0x009e01ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TLDS(lowered.instructions.front());
    u32 fetches = 0;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageFetch) {
            ++fetches;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type == Shader::TextureType::Color1D);
            REQUIRE(inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister);
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg{4});
        }
    }
    REQUIRE(fetches == 1);
}

TEST_CASE("Native scalar attribute stores accept the zero register without wrapping", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0xff007322, 0x000070ff, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.AST(lowered.instructions.front());
    u32 stores = 0;
    for (const auto& instruction : block.Instructions()) {
        if (instruction.GetOpcode() != IR::Opcode::SetAttribute) continue;
        ++stores;
        REQUIRE(instruction.Arg(0).Attribute() == IR::Attribute::PositionX);
    }
    REQUIRE(stores == 1);
    REQUIRE(EvaluateVertex(block, {8.0f, 9.0f}, {}) == std::array<float, 2>{0.0f, 0.0f});
    auto broken = input;
    broken[0].words[2] |= 1U << 10;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
}

TEST_CASE("Native call and return preserve values through the actual shader frontend", "[native-shader]") {
    std::array<Native, 9> input{{
        {{0x00007802, 0x40000000, 0x00000f00, 0}}, // argument 2.0
        {{0x00377802, 0x00000030, 0x00000f00, 0}}, // native return PC
        {{0x00007944, 0x00000020, 0x03c00000, 0}}, // call offset0x50
        {{0xff007322, 0x00007003, 0x000000ff, 0}},
        VertexTransform.back(),
        {{0x00037421, 0x40400000, 0x00010000, 0}}, // callee: add 3.0
        {{0x00087202, 0x00000037, 0x00000f00, 0}},
        {{0x00097802, 0x00000000, 0x00000f00, 0}},
        {{0x08007950, 0xffffff70, 0x03c3ffff, 0}},
    }};
    const bool alternate_link = GENERATE(false, true);
    if (alternate_link) {
        input[1].words[0] = 0x00397802;
        input[6] = Native{{0x00007202, 0x00000039, 0x00000f00, 0}};
        input[7].words[0] = 0x00017802;
        input[8].words[0] = 0x00007950;
    }
    const bool inplace = GENERATE(false, true);
    if (inplace) {
        const u32 link = alternate_link ? 10U : 8U;
        input[1].words[0] = 0x7802U | (link << 16);
        input[6] = Native{{0x7947, 0, 0x03800000, 0}};
        input[7].words[0] = 0x7802U | ((link + 1) << 16);
        input[8].words[0] = 0x7950U | (link << 24);
    }
    const bool synchronize = GENERATE(false, true);
    std::vector<Native> program_input(input.begin(), input.end());
    if (synchronize) {
        program_input.insert(program_input.begin(), Native{{0x27945, 64, 0x03800000, 0}});
        program_input.insert(program_input.begin() + 4, Native{{0x27941, 0, 0x03800000, 0}});
        program_input.insert(program_input.begin() + 5, Native{{0x7948, 0xffffffff, 0x03800000, 0}});
        program_input[2].words[1] = 64;
        program_input[3].words[1] = 64;
        program_input.back().words[1] = 0xffffff40;
    }
    const auto lowered = Shader::Ampere::LowerToMaxwell(program_input);
    REQUIRE(lowered);
    REQUIRE(lowered.source_size == program_input.size() * 16);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::span{input}.first(5)));
    auto broken = input;
    broken[1].words[1] += 16;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken.back().words[1] += 16;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    if (inplace) {
        auto corrupt = input;
        corrupt[5].words[0] = (corrupt[5].words[0] & ~0xff0000U) | (input[1].words[0] & 0xff0000U);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(corrupt));
        corrupt = input;
        corrupt[6].words[1] = 16;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(corrupt));
    }
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class CallEnvironment final : public EmptyEnvironment {
    public:
        explicit CallEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    REQUIRE(cfg.Functions().size() == 2);
    Shader::HostTranslateInfo host;
    const auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    u32 stores = 0;
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) continue;
            ++stores;
            REQUIRE(inst.Arg(1).IsImmediate());
            REQUIRE(inst.Arg(1).F32() == 5.0f);
        }
    }
    REQUIRE(stores == 1);
}

TEST_CASE("Native barrier cancellation exits the inner scope without revisiting it", "[native-shader]") {
    std::array<Native, 10> input{{
        {{0x7945, 128, 0x03800000, 0}},
        {{0x17945, 80, 0x03800000, 0}},
        {{0x18942, 0, 0x03800000, 0}},
        {{0xff028424, 1, 0x078e00ff, 0}},
        {{0x8947, 48, 0x03800000, 0}},
        {{0x27802, 2, 0xf00, 0}},
        {{0x17941, 0, 0x03800000, 0}},
        {{0x27802, 3, 0xf00, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto broken = input;
    broken[3].words[0] ^= 0x8000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[4].words[1] = 32;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[2].words[0] = 0x8942;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class BarrierEnvironment final : public EmptyEnvironment {
    public:
        explicit BarrierEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const u32 offset = lowered.source_instruction_offsets[7];
    const Shader::Maxwell::Location continuation{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    for (bool predicate : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool visited_continuation = false, exited = false;
        std::string trace;
        for (u32 step = 0; step < 64 && block; ++step) {
            trace += std::to_string(block->begin.Offset()) + ":" + std::to_string(block->end.Offset()) + " ";
            visited_continuation |= !block->begin.IsVirtual() && block->Contains(continuation);
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            const bool taken = (pred == IR::Pred::PT || predicate) != inverted;
            block = taken ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        INFO("path=" << trace << " marker=" << continuation.Offset());
        REQUIRE(visited_continuation == predicate);
    }
}

TEST_CASE("Native cancellation of two nested barriers preserves the outer join", "[native-shader]") {
    const std::array<Native, 15> input{{
        {{0x27945, 208, 0x03800000, 0}},
        {{0x7945, 160, 0x03800000, 0}},
        {{0x17945, 96, 0x03800000, 0}},
        {{0x18942, 0, 0x03800000, 0}},
        {{0x8942, 0, 0x03800000, 0}},
        {{0x8947, 96, 0x03800000, 0}},
        {{0x27802, 2, 0xf00, 0}},
        {{0x7918, 0, 0, 0}},
        {{0x17941, 0, 0x03800000, 0}},
        {{0x27802, 3, 0xf00, 0}},
        {{0x7918, 0, 0, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        {{0x37802, 4, 0xf00, 0}},
        {{0x27941, 0, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto invalid = input;
    invalid[4].words[0] ^= 0x8000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[5].words[1] = 16;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class NestedBarrierEnvironment final : public EmptyEnvironment {
    public:
        explicit NestedBarrierEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const auto location = [&](u32 index) {
        const u32 offset = lowered.source_instruction_offsets[index];
        return Shader::Maxwell::Location{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    };
    for (bool predicate : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool continued{}, cleaned{}, exited{};
        for (u32 step = 0; step < 64 && block; ++step) {
            continued |= !block->begin.IsVirtual() && block->Contains(location(9));
            cleaned |= !block->begin.IsVirtual() && block->Contains(location(12));
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            block = ((pred == IR::Pred::PT || predicate) != inverted) ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        REQUIRE(cleaned);
        REQUIRE(continued == predicate);
    }
}

TEST_CASE("Native full warp synchronization retains ordering after reconvergence", "[native-shader]") {
    std::array<Native, 4> input{{
        {{0x7945, 16, 0x03800000, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        {{0x7948, 0xffffffff, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const u32 token = GENERATE(0U, 2U, 7U);
    input[0].words[0] |= token << 16;
    input[1].words[0] |= token << 16;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.MEMBAR(lowered.instructions.at(lowered.source_instruction_offsets[2]));
    REQUIRE(block.Instructions().front().GetOpcode() == IR::Opcode::DeviceMemoryBarrier);
    auto broken = input;
    broken[2].words[1] = 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[0].words[1] = 32;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[1].words[0] = 0x7947;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
}

TEST_CASE("Native EXIT retains its execution and branch predicates", "[native-shader]") {
    const u32 execution = GENERATE(7U, 9U);
    const std::array<Native, 3> input{{
        {{0x94dU | (execution << 12), 0, 0, 0}},
        {{0x27802, 1, 0xf00, 0}},
        VertexTransform.back(),
    }};
    Shader::Ampere::ProgramCaptureState capture;
    REQUIRE(capture.Observe(input[0], 0) == Shader::Ampere::ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(input[1], 1) == Shader::Ampere::ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(input[2], 2) == Shader::Ampere::ProgramCaptureState::Result::Complete);
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.source_size == sizeof(input));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class ExitEnvironment final : public EmptyEnvironment {
    public:
        explicit ExitEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    const u32 offset = lowered.source_instruction_offsets[1];
    const Shader::Maxwell::Location continuation{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    for (bool p0 : {false, true}) for (bool p1 : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool continued = false, exited = false;
        for (u32 step = 0; step < 32 && block; ++step) {
            continued |= !block->begin.IsVirtual() && block->Contains(continuation);
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::P1 || pred == IR::Pred::PT));
            const bool taken = (pred == IR::Pred::PT ? true : pred == IR::Pred::P0 ? p0 : p1) != inverted;
            block = taken ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        REQUIRE(continued == !(p0 && (execution == 7 || !p1)));
    }
}

TEST_CASE("Native reconvergence register names are independent of nesting depth", "[native-shader]") {
    const u32 outer = GENERATE(1U, 4U), inner = GENERATE(0U, 2U);
    const std::array<Native, 8> input{{
        {{0x27802, 0x3f800000, 0xf00, 0}},
        {{0x7945U | (outer << 16), 64, 0x03800000, 0}},
        {{0x7945U | (inner << 16), 16, 0x03800000, 0}},
        {{0x7941U | (inner << 16), 0, 0x03800000, 0}},
        {{0x27802, 0x40000000, 0xf00, 0}},
        {{0x7941U | (outer << 16), 0, 0x03800000, 0}},
        {{0xff007322, 0x7002, 0xff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto broken = input;
    broken[2].words[0] = 0x7945U | (outer << 16);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[3].words[0] = 0x7941U | (outer << 16);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class ScopeEnvironment final : public EmptyEnvironment {
    public:
        explicit ScopeEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    Shader::HostTranslateInfo host;
    const auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    u32 stores = 0;
    for (const auto* block : program.blocks) for (const auto& instruction : block->Instructions()) {
        if (instruction.GetOpcode() != IR::Opcode::SetAttribute) continue;
        ++stores;
        REQUIRE(instruction.Arg(1).IsImmediate());
        REQUIRE(instruction.Arg(1).F32() == 2.0f);
    }
    REQUIRE(stores == 1);
}

TEST_CASE("Native scaled multiplication preserves captured power-of-two scales", "[native-shader]") {
    std::array<Native, 4> input{{
        VertexTransform.front(),
        {{0x00007220, 0x00000001, 0x00310000, 0}},
        {{0xff007322, 0x00007000, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const u32 scale = GENERATE(3U, 4U, 5U, 6U);
    input[1].words[2] = (scale << 20) | 0x10000;
    const float factor = scale == 3 ? 0.5f : scale == 5 ? 2.0f : scale == 6 ? 4.0f : 1.0f;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.ALD(lowered.instructions[0]);
    visitor.FMUL_reg(lowered.instructions[1]);
    visitor.AST(lowered.instructions[2]);
    REQUIRE(EvaluateVertex(block, {8, 4}, {}) == std::array<float, 2>{32 * factor, 0});
    REQUIRE(EvaluateVertex(block, {-2, 3}, {}) == std::array<float, 2>{-6 * factor, 0});
}

TEST_CASE("Native integer multiply preserves high words and modular multiply-add", "[native-shader]") {
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
    Native instruction{{0x00037827, 0xcccccccd, 0x078e00ff, 0}};
    if (mode == 1) instruction = Native{{0x00037824, 0xfffffff6, 0x078e0202, 0}};
    if (mode == 2) instruction = Native{{0x00037a24, 0x00c00000, 0x078e0202, 0}};
    if (mode == 4) instruction = Native{{0x00037811, 0x00000070, 0x078e20ff, 0}};
    if (mode == 3) instruction = Native{{0x00037224, 0x00000001, 0x078e0202, 0}};
    if (mode == 5) instruction = Native{{0x00037824, 0x00000001, 0x078e0a02, 0}};
    const std::array<Native, 2> input{{instruction, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::XMAD_imm: visitor.XMAD_imm(word); break;
        case Opcode::XMAD_reg: visitor.XMAD_reg(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::ISCADD_reg: visitor.ISCADD_reg(word); break;
        case Opcode::SHR_imm: visitor.SHR_imm(word); break;
        case Opcode::IADD_reg: visitor.IADD_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected canonical integer multiplication instruction");
        }
    }
    for (u32 a : {0U, 1U, 9U, 10U, 65535U, 65536U, 0x80000000U, 0xfffffffeU, 0xffffffffU, 0x12345678U}) {
        std::array<u32, 256> regs{};
        regs[0] = a;
        regs[1] = 0x76543210;
        regs[2] = 0xfedcba98;
        std::unordered_map<const IR::Inst*, u32> values;
        const auto get = [&](const IR::Value& value) -> u32 {
            if (!value.IsImmediate()) return values.at(value.Inst());
            return value.Type() == IR::Type::U1 ? static_cast<u32>(value.U1()) : value.U32();
        };
        for (const auto& inst : block.Instructions()) {
            const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetRegister: values[&inst] = regs[static_cast<u32>(inst.Arg(0).Reg())]; break;
            case IR::Opcode::SetRegister: regs[static_cast<u32>(inst.Arg(0).Reg())] = arg(1); break;
            case IR::Opcode::GetCbufU32:
                REQUIRE(arg(0) == 3);
                REQUIRE(arg(1) == 0);
                values[&inst] = 0x76543210;
                break;
            case IR::Opcode::BitFieldUExtract: values[&inst] = (arg(0) >> arg(1)) & ((1U << arg(2)) - 1); break;
            case IR::Opcode::INeg32: values[&inst] = 0U - arg(0); break;
            case IR::Opcode::IMul32: values[&inst] = arg(0) * arg(1); break;
            case IR::Opcode::IAdd32: values[&inst] = arg(0) + arg(1); break;
            case IR::Opcode::ShiftLeftLogical32: values[&inst] = arg(0) << arg(1); break;
            case IR::Opcode::ShiftRightLogical32: values[&inst] = arg(0) >> arg(1); break;
            case IR::Opcode::ULessThan: values[&inst] = arg(0) < arg(1); break;
            case IR::Opcode::SLessThan: values[&inst] = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1)); break;
            case IR::Opcode::SelectU32: values[&inst] = arg(0) ? arg(1) : arg(2); break;
            default: FAIL("Unexpected multiplication IR operation " << IR::NameOf(inst.GetOpcode()));
            }
        }
        const u32 b = mode == 0 ? 0xcccccccd : mode == 1 ? 0xfffffff6 : 0x76543210;
        const u64 product = u64{a} * b;
        const u32 expected = mode == 5 ? a - 0xfedcba98U : mode == 4 ? (a << 4) + 0x70U : mode == 0 ? static_cast<u32>(product >> 32)
                                       : static_cast<u32>(product + 0xfedcba98U);
        REQUIRE(regs[3] == expected);
    }
}

TEST_CASE("Native uniform constants keep their indexed load and arithmetic values", "[native-shader]") {
    const std::array<Native, 7> input{{
        {{0xff007321, 0x000080ff, 0x00000000, 0}},
        {{0x00047ab9, 0x00c00000, 0x00000800, 0}},
        {{0x04047899, 0x00000002, 0x0800063f, 0}},
        {{0x04057abb, 0x00c00000, 0x08000800, 0}},
        {{0x00007c20, 0x00000005, 0x08410000, 0}},
        {{0x00007e21, 0x80000005, 0x08010000, 0}},
        VertexTransform.back(),
    }};
    auto code = input;
    // Store the computed scalar instead of ending immediately.
    std::vector<Native> program(code.begin(), code.end() - 1);
    program.push_back(Native{{0xff007322, 0x00007000, 0x000000ff, 0}});
    program.push_back(VertexTransform.back());
    const auto lowered = Shader::Ampere::LowerToMaxwell(program);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::ALD: visitor.ALD(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::LOP32I: visitor.LOP32I(word); break;
        case Opcode::SHL_imm: visitor.SHL_imm(word); break;
        case Opcode::LDC: visitor.LDC(word); break;
        case Opcode::FMUL_reg: visitor.FMUL_reg(word); break;
        case Opcode::FADD_reg: visitor.FADD_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected uniform instruction lowering");
        }
    }
    const std::array<float, 4> constants{std::bit_cast<float>(1U), 3.0f, 7.0f, 9.0f};
    REQUIRE(EvaluateVertex(block, {2, 0}, constants) == std::array<float, 2>{3, 0});
    REQUIRE(EvaluateVertex(block, {4, 0}, constants) == std::array<float, 2>{9, 0});
    auto conflict = program;
    conflict[0].words[0] = 0xfff87321; // Native R248 conflicts with the reserved uniform bank.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(conflict));
}

TEST_CASE("Native predicate-output LUTs preserve boolean bit tests", "[native-shader]") {
    const std::array<Native, 3> input{{
        {{0x00ff7812, 0x00000001, 0x0780c0ff, 0}},
        {{0x01ff7812, 0x00000001, 0x07824802, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::LOP_reg: visitor.LOP_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected predicate LUT lowering");
        }
    }
    for (u32 a = 0; a < 4; ++a) for (u32 b = 0; b < 4; ++b) for (u32 c = 0; c < 4; ++c) {
        std::array<u32, 256> regs{};
        regs[0] = a; regs[1] = b; regs[2] = c;
        std::array<bool, 7> predicates{false, false, true, false, true, false, true};
        std::unordered_map<const IR::Inst*, u32> values;
        const auto get = [&](const IR::Value& value) -> u32 {
            if (!value.IsImmediate()) return values.at(value.Inst());
            return value.Type() == IR::Type::U1 ? static_cast<u32>(value.U1()) : value.U32();
        };
        for (const auto& inst : block.Instructions()) {
            const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetRegister: values[&inst] = regs[static_cast<u32>(inst.Arg(0).Reg())]; break;
            case IR::Opcode::SetRegister: regs[static_cast<u32>(inst.Arg(0).Reg())] = arg(1); break;
            case IR::Opcode::BitwiseAnd32: values[&inst] = arg(0) & arg(1); break;
            case IR::Opcode::BitwiseOr32: values[&inst] = arg(0) | arg(1); break;
            case IR::Opcode::BitwiseXor32: values[&inst] = arg(0) ^ arg(1); break;
            case IR::Opcode::INotEqual: values[&inst] = arg(0) != arg(1); break;
            case IR::Opcode::SetPred: predicates[static_cast<u32>(inst.Arg(0).Pred())] = arg(1) != 0; break;
            default: FAIL("Unexpected predicate LUT IR " << IR::NameOf(inst.GetOpcode()));
            }
        }
        REQUIRE(predicates[0] == ((a & 1) != 0));
        REQUIRE(predicates[1] == (((b ^ c) & 1) != 0));
        REQUIRE(std::array<bool, 5>{predicates[2], predicates[3], predicates[4], predicates[5], predicates[6]} ==
                std::array<bool, 5>{true, false, true, false, true});
    }
}

TEST_CASE("Native cube sampling preserves XYZ, explicit LOD, and split color destinations", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0x04007b60, 0x70800808, 0x059e0703, 0}},
        VertexTransform.back(),
    }};
    const u32 binding = GENERATE(0U, 2U);
    input[0].words[1] = (input[0].words[1] & ~(31U << 22)) | (binding << 22);
    const bool zero_lod = GENERATE(false, true);
    const bool volume = GENERATE(false, true);
    if (volume) {
        input[0].words[1] = (input[0].words[1] & ~(7U << 29)) | (2U << 29);
        input[0].words[2] = (input[0].words[2] & ~(7U << 23)) | (1U << 23);
    }
    if (zero_lod) {
        input[0].words[2] = (input[0].words[2] & ~(7U << 23)) | (1U << 23);
        input[0].words[1] = (input[0].words[1] & ~0xffU) | 9;
    }
    const bool rgba = GENERATE(false, true);
    if (rgba) input[0].words[2] = (input[0].words[2] & ~0xffffU) | 0x0f02;
    const bool pair = GENERATE(false, true);
    if (pair) input[0].words[2] = (input[0].words[2] & ~0xffffU) | 0x03ff;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == binding);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::TEX: visitor.TEX(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected cube sampling lowering");
        }
    }
    u32 samples = 0;
    std::vector<std::pair<u32, u32>> copies;
    std::vector<u32> writes, channels;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4) channels.push_back(inst.Arg(1).U32());
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            const u32 target = static_cast<u32>(inst.Arg(0).Reg());
            if (target < 248) writes.push_back(target);
            if (inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
                copies.emplace_back(target, static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
        }
        if (inst.GetOpcode() == IR::Opcode::BoundImageSampleExplicitLod) {
            ++samples;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type == (volume ? Shader::TextureType::Color3D : Shader::TextureType::ColorCube));
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructF32x3);
            for (u32 i = 0; i < 3; ++i)
                REQUIRE(coords->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250 + i});
            if (volume || zero_lod) REQUIRE(inst.Arg(2).F32() == 0.0f);
            else REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{9});
        }
    }
    REQUIRE(samples == 1);
    REQUIRE(writes == (pair ? std::vector<u32>{0, 1} : rgba ? std::vector<u32>{0, 1, 2, 3} : std::vector<u32>{0, 1, 3}));
    REQUIRE(channels == (pair ? std::vector<u32>{0, 1} : rgba ? std::vector<u32>{0, 1, 2, 3} : std::vector<u32>{0, 1, 2}));
    REQUIRE(copies == (pair ? std::vector<std::pair<u32, u32>>{{250, 4}, {251, 5}, {252, zero_lod ? 9U : 8U}, {0, 250}, {1, 251}} : rgba ? std::vector<std::pair<u32, u32>>{{250, 4}, {251, 5}, {252, zero_lod ? 9U : 8U}, {0, 250}, {1, 251}, {2, 252}, {3, 253}} :
                             std::vector<std::pair<u32, u32>>{{250, 4}, {251, 5}, {252, zero_lod ? 9U : 8U}, {0, 250}, {1, 251}, {3, 252}}));
}

TEST_CASE("Native branch maps survive code caching and reject invalid addresses", "[native-shader]") {
    std::vector<u64> code(64);
    const std::array<u32, 4> offsets{0, 2, 4, 8};
    Shader::Ampere::AppendNativeAddressMap(code, offsets);
    REQUIRE(Shader::Ampere::HasNativeAddressMap(code));
    REQUIRE(Shader::Ampere::NativeProgramAddress(code, 0) == 0x50U);
    REQUIRE(Shader::Ampere::NativeProgramAddress(code, 16) == 0x68U);
    const auto restored = code;
    REQUIRE(Shader::Ampere::NativeProgramAddress(restored, 48) == 0xa8U);
    REQUIRE_FALSE(Shader::Ampere::NativeProgramAddress(code, 1));
    REQUIRE_FALSE(Shader::Ampere::NativeProgramAddress(code, 64));
    code[code.size() - 2] = 100000;
    REQUIRE_FALSE(Shader::Ampere::NativeProgramAddress(code, 0));
}

TEST_CASE("Native computed jumps retain table values while mapping code destinations", "[native-shader]") {
    const std::array<Native, 12> input{{
        {{0xff007321, 0x0002fcff, 0x00000000, 0}},
        {{0x00007817, 0x00000001, 0x03800000, 0}},
        {{0x00027819, 0x00000002, 0x000006ff, 0}},
        {{0x02007b82, 0x00400000, 0x00000800, 0}},
        {{0xff037421, 0x3f000000, 0x00010000, 0}},
        {{0x00007949, 0xffffffa0, 0x0383ffff, 0}},
        {{0x00007802, 0x3f800000, 0x00000f00, 0}},
        {{0x00007947, 0x00000020, 0x03800000, 0}},
        {{0x00007802, 0x40000000, 0x00000f00, 0}},
        {{0x00007947, 0x00000000, 0x03800000, 0}},
        {{0xff007322, 0x00007000, 0x000000ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    class BranchEnvironment final : public EmptyEnvironment {
    public:
        explicit BranchEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 1);
            REQUIRE((offset == 0 || offset == 4));
            return offset == 0 ? 0x60 : 0x80;
        }
        u32 TranslateProgramAddress(u32 address) const override {
            const auto target = Shader::Ampere::NativeProgramAddress(code, address);
            if (!target) throw std::logic_error("invalid synthetic branch target");
            return *target;
        }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    u32 jumps = 0;
    for (const auto& block : cfg.Functions().front().blocks) {
        if (block.end_class != Shader::Maxwell::Flow::EndClass::IndirectBranch) continue;
        ++jumps;
        REQUIRE(block.branch_offset == 0);
        REQUIRE(block.indirect_branches.size() == 2);
        for (const auto& branch : block.indirect_branches) {
            REQUIRE((branch.address == 0x60 || branch.address == 0x80));
            REQUIRE(branch.block->begin.Offset() == env.TranslateProgramAddress(branch.address));
            REQUIRE(branch.block->begin.Offset() != branch.address);
        }
    }
    REQUIRE(jumps == 1);
    Shader::HostTranslateInfo host;
    const auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    REQUIRE_FALSE(program.blocks.empty());
}

TEST_CASE("Native forward branches retain paths beyond an early unconditional exit", "[native-shader]") {
    const Native exit{{0x0000794d, 0, 0x03800000, 0x000fea00}};
    const std::array<Native, 4> source{{
        {{0x00000947, 0x20, 0x03800000, 0x000fea00}},
        exit,
        {{0x00017802, 42, 0x00000f00, 0x000fe400}},
        exit,
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(source);
    REQUIRE(lowered);
    REQUIRE(lowered.source_size == sizeof(source));
    REQUIRE(lowered.source_instruction_offsets.size() == source.size());
    REQUIRE(lowered.instructions.size() == 4);
    const auto truncated = Shader::Ampere::LowerToMaxwell(std::span{source}.first(2));
    REQUIRE_FALSE(truncated);
    REQUIRE(truncated.instructions.empty());
}

TEST_CASE("Native register-to-predicate writes only masked predicate bits", "[native-shader]") {
    const std::array<Native, 2> source{{{{0x05007804, 7, 0, 0x000fe200}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(source);
    REQUIRE(lowered);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[0]) == Shader::Maxwell::Opcode::R2P_imm);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.R2P_imm(lowered.instructions[0]);
    for (u32 value = 0; value < 128; ++value) {
        std::array<bool, 7> predicates{true, false, true, false, true, false, true};
        std::unordered_map<const IR::Inst*, u32> values;
        const auto get = [&](const IR::Value& v) -> u32 {
            if (!v.IsImmediate()) return values.at(v.Inst());
            return v.Type() == IR::Type::U1 ? static_cast<u32>(v.U1()) : v.U32();
        };
        for (const auto& inst : block.Instructions()) {
            const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetRegister:
                REQUIRE(static_cast<u32>(inst.Arg(0).Reg()) == 5);
                values[&inst] = value; break;
            case IR::Opcode::GetPred: values[&inst] = predicates[static_cast<u32>(inst.Arg(0).Pred())]; break;
            case IR::Opcode::SetPred: predicates[static_cast<u32>(inst.Arg(0).Pred())] = arg(1) != 0; break;
            case IR::Opcode::BitFieldUExtract: values[&inst] = (arg(0) >> arg(1)) & ((1U << arg(2)) - 1); break;
            case IR::Opcode::GetZeroFromOp: values[&inst] = arg(0) == 0; break;
            case IR::Opcode::IEqual: values[&inst] = arg(0) == arg(1); break;
            case IR::Opcode::LogicalNot: values[&inst] = !arg(0); break;
            case IR::Opcode::SelectU1: values[&inst] = arg(0) ? arg(1) : arg(2); break;
            default: FAIL("Unexpected R2P IR " << IR::NameOf(inst.GetOpcode()));
            }
        }
        for (u32 bit = 0; bit < 7; ++bit)
            REQUIRE(predicates[bit] == (bit < 3 ? bool(value & (1U << bit)) : bool(bit % 2 == 0)));
    }
    auto invalid = source;
    invalid[0].words[2] = 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native 2D explicit LOD sampling preserves its paired coordinates", "[native-shader]") {
    const std::array<Native, 2> input{{{{0xc007b60, 0x30800812, 0x59e070b, 0x100f4200}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TEXS(lowered.instructions[0]);
    u32 samples = 0;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::BoundImageSampleExplicitLod) continue;
        ++samples;
        REQUIRE(inst.Flags<IR::TextureInstInfo>().type == Shader::TextureType::Color2D);
        const auto* coords = inst.Arg(1).Inst();
        REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructF32x2);
        for (u32 i = 0; i < 2; ++i)
            REQUIRE(coords->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{12 + i});
        REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{18});
    }
    REQUIRE(samples == 1);
}

namespace {
using Warp = std::array<u32, 32>;
void ExecuteNativeWarp(IR::Block& block, u32 active,
                       std::array<std::array<u32, 256>, 32>& registers,
                       std::array<std::array<bool, 7>, 32>& predicates) {
    std::unordered_map<const IR::Inst*, Warp> values;
    const auto get = [&](const IR::Value& value) -> Warp {
        if (!value.IsImmediate()) return values.at(value.Inst());
        Warp result;
        result.fill(value.Type() == IR::Type::U1 ? static_cast<u32>(value.U1()) : value.U32());
        return result;
    };
    for (const auto& inst : block.Instructions()) {
        std::array<Warp, 4> args{};
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            if (inst.Arg(i).Type() != IR::Type::Reg && inst.Arg(i).Type() != IR::Type::Pred)
                args[i] = get(inst.Arg(i));
        }
        Warp result{};
        const auto op = inst.GetOpcode();
        if (op == IR::Opcode::SubgroupBallot || op == IR::Opcode::VoteAny || op == IR::Opcode::VoteAll) {
            u32 mask = 0;
            for (u32 lane = 0; lane < 32; ++lane)
                if ((active & (1U << lane)) && args[0][lane]) mask |= 1U << lane;
            result.fill(op == IR::Opcode::SubgroupBallot ? mask : op == IR::Opcode::VoteAny ? mask != 0 : mask == active);
        } else {
            for (u32 lane = 0; lane < 32; ++lane) {
                const auto a = args[0][lane], b = args[1][lane], c = args[2][lane];
                switch (op) {
                case IR::Opcode::GetRegister: result[lane] = static_cast<u32>(inst.Arg(0).Reg()) == 255 ? 0 : registers[lane][static_cast<u32>(inst.Arg(0).Reg())]; break;
                case IR::Opcode::SetRegister:
                    if ((active & (1U << lane)) && static_cast<u32>(inst.Arg(0).Reg()) != 255) registers[lane][static_cast<u32>(inst.Arg(0).Reg())] = b;
                    break;
                case IR::Opcode::GetPred: result[lane] = predicates[lane][static_cast<u32>(inst.Arg(0).Pred())]; break;
                case IR::Opcode::SetPred:
                    if (active & (1U << lane)) predicates[lane][static_cast<u32>(inst.Arg(0).Pred())] = b != 0;
                    break;
                case IR::Opcode::BitCastF32U32: case IR::Opcode::BitCastU32F32: result[lane] = a; break;
                case IR::Opcode::SelectU32: case IR::Opcode::SelectU1: result[lane] = a ? b : c; break;
                case IR::Opcode::BitwiseAnd32: result[lane] = a & b; break;
                case IR::Opcode::BitwiseOr32: result[lane] = a | b; break;
                case IR::Opcode::BitwiseNot32: result[lane] = ~a; break;
                case IR::Opcode::UMin32: result[lane] = std::min(a, b); break;
                case IR::Opcode::UMax32: result[lane] = std::max(a, b); break;
                case IR::Opcode::ShiftRightLogical32: result[lane] = b < 32 ? a >> b : 0; break;
                case IR::Opcode::ShiftLeftLogical32: result[lane] = b < 32 ? a << b : 0; break;
                case IR::Opcode::INeg32: result[lane] = 0U - a; break;
                case IR::Opcode::IAdd32: result[lane] = a + b; break;
                case IR::Opcode::FindUMsb32: result[lane] = a ? 31U - std::countl_zero(a) : ~0U; break;
                case IR::Opcode::BitFieldUExtract: result[lane] = (a >> b) & ((1U << c) - 1); break;
                case IR::Opcode::ULessThan: result[lane] = a < b; break;
                case IR::Opcode::SLessThan: result[lane] = static_cast<s32>(a) < static_cast<s32>(b); break;
                case IR::Opcode::IEqual: result[lane] = a == b; break;
                case IR::Opcode::GetZeroFromOp: result[lane] = a == 0; break;
                case IR::Opcode::LogicalAnd: result[lane] = a && b; break;
                case IR::Opcode::LogicalNot: result[lane] = !a; break;
                case IR::Opcode::GetInBoundsFromOp: result[lane] = get(inst.Arg(0).Inst()->Arg(1))[lane] <= 31; break;
                case IR::Opcode::FSwizzleAdd: {
                    const u32 mode = (c >> ((lane & 3) * 2)) & 3;
                    const float x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
                    const float ma = mode == 0 || mode == 2 ? -1.0f : mode == 1 ? 1.0f : 0.0f;
                    const float mb = mode == 0 || mode == 1 ? -1.0f : mode == 2 ? 1.0f : 0.0f;
                    result[lane] = std::bit_cast<u32>(x * ma + y * mb);
                    break;
                }
                case IR::Opcode::ShuffleButterfly: result[lane] = args[0][lane ^ b]; break;
                case IR::Opcode::ShuffleIndex: result[lane] = (active & (1U << (b & 31))) ? args[0][b & 31] : 0xdeadbeefU; break;
                default: FAIL("Unexpected warp-uniform IR " << IR::NameOf(op));
                }
            }
        }
        values.emplace(&inst, result);
    }
}
}

TEST_CASE("Native warp uniform transfers use the first selected active lane and preserve predicates", "[native-shader]") {
    const u32 operation = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
    const Native instruction = operation < 2
        ? Native{{operation == 0 ? 0x020513c2U : 0x020593c2U, 0, 0x000e0000, 0}}
        : operation < 4 ? Native{{0x003f7886, 0, operation == 2 ? 0x00800100U : 0x00800000U, 0}}
        : Native{{0x0000781c, 0, operation == 4 ? 0x03f2f008U : operation == 5 ? 0x03f0f070U : operation == 6 ? 0x03f0e170U : 0x00f41070U, 0}};
    const std::array<Native, 2> source{{instruction, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(source);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::PSETP: visitor.PSETP(word); break;
        case Opcode::SHR_imm: visitor.SHR_imm(word); break;
        case Opcode::SHL_imm: visitor.SHL_imm(word); break;
        case Opcode::MOV32I: visitor.MOV32I(word); break;
        case Opcode::P2R_imm: visitor.P2R_imm(word); break;
        case Opcode::R2P_imm: visitor.R2P_imm(word); break;
        case Opcode::VOTE: visitor.VOTE(word); break;
        case Opcode::IADD_reg: visitor.IADD_reg(word); break;
        case Opcode::LOP_reg: visitor.LOP_reg(word); break;
        case Opcode::FLO_reg: visitor.FLO_reg(word); break;
        case Opcode::SHFL: visitor.SHFL(word); break;
        case Opcode::SEL_reg: visitor.SEL_reg(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected warp-uniform lowering");
        }
    }
    using Warp = std::array<u32, 32>;
    for (const u32 active : {1U, 0x80000000U, 0x80000001U, 0x55555555U, 0xffffffffU}) {
        for (const u32 selected : {0U, 1U, 0x80000000U, 0xaaaaaaaaU, 0xffffffffU}) {
            std::array<std::array<u32, 256>, 32> registers{};
            std::array<std::array<bool, 7>, 32> predicates{};
            for (u32 lane = 0; lane < 32; ++lane) {
                registers[lane][2] = 100 + lane * 17;
                registers[lane][221] = 0xcafebeef;
                registers[lane][239] = (selected & (1U << lane)) ? 64 : 0;
                for (u32 p = 0; p < 7; ++p) predicates[lane][p] = ((lane + p) & 1) != 0;
                predicates[lane][1] = ((selected & (1U << lane)) != 0) != (operation == 4);
            }
            const auto original_predicates = predicates;
            ExecuteNativeWarp(block, active, registers, predicates);
            const u32 ballot = active & (operation == 1 ? ~selected : selected);
            const u32 first = ballot ? std::countr_zero(ballot) : 0;
            for (u32 lane = 0; lane < 32; ++lane) {
                if (!(active & (1U << lane))) continue;
                auto expected_predicates = original_predicates[lane];
                if (operation == 7) expected_predicates[2] = original_predicates[lane][1] && original_predicates[lane][0];
                if (operation == 4) expected_predicates[1] = (selected & (1U << lane)) != 0;
                if (operation == 5 || operation == 6) expected_predicates[0] = operation == 5;
                REQUIRE(predicates[lane] == expected_predicates);
                if (operation < 2) REQUIRE(registers[lane][221] == (ballot ? 100 + first * 17 : 0xcafebeef));
                else if (operation < 4) REQUIRE(registers[lane][239] == (operation == 2 ? (ballot ? 64U : 0U) : (ballot == active ? 64U : 0U)));
            }
        }
    }
}

TEST_CASE("Native uniform 64-bit loads populate consecutive registers", "[native-shader]") {
    const std::array<Native, 6> input{{
        {{0x00057ab9, 0x00c00000, 0x00000800, 0}},
        {{0x05067abb, 0x00c00100, 0x08000a00, 0}},
        {{0x00007c02, 6, 0x08000f00, 0}},
        {{0x00017c02, 7, 0x08000f00, 0}},
        {{0xff007322, 0x00007000, 0x000004ff, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (const auto word : lowered.instructions) {
        using Shader::Maxwell::Opcode;
        switch (Shader::Maxwell::Decode(word)) {
        case Opcode::MOV_cbuf: visitor.MOV_cbuf(word); break;
        case Opcode::MOV_reg: visitor.MOV_reg(word); break;
        case Opcode::LDC: visitor.LDC(word); break;
        case Opcode::AST: visitor.AST(word); break;
        case Opcode::EXIT: break;
        default: FAIL("Unexpected uniform pair lowering");
        }
    }
    REQUIRE(EvaluateVertex(block, {}, {std::bit_cast<float>(0U), 3, 7, 9}) == std::array<float, 2>{3, 7});
}

TEST_CASE("Native texture dimension queries retain mip input and width-height outputs", "[native-shader]") {
    const u32 binding = GENERATE(0U, 2U, 17U);
    const std::array<Native, 2> source{{{{0x03027b6f, 0x00000800U | (binding << 22), 0x000003ff, 0}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(source);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == binding);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[0]) == Shader::Maxwell::Opcode::TXQ);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.TXQ(lowered.instructions[0]);
    std::vector<std::pair<u32, u32>> outputs;
    u32 queries = 0;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageQueryDimensions) {
            ++queries;
            REQUIRE(inst.Arg(0).U32() == 32);
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg{3});
            REQUIRE(inst.Arg(2).U1());
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            const auto* extract = inst.Arg(1).Inst();
            REQUIRE(extract->GetOpcode() == IR::Opcode::CompositeExtractU32x4);
            outputs.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), extract->Arg(1).U32());
        }
    }
    REQUIRE(queries == 1);
    REQUIRE(outputs == std::vector<std::pair<u32, u32>>{{2, 0}, {3, 1}});
    auto invalid = source;
    invalid[0].words[1] |= 1U << 30;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = source;
    invalid[0].words[1] = (invalid[0].words[1] & ~(31U << 22)) | (18U << 22);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    const std::array<Native, 3> conflict{{source[0], source[0], source[1]}};
    auto mixed = conflict;
    mixed[1].words[1] ^= 1U << 22;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(mixed));
}

TEST_CASE("Native scalar global stores retain both uniform address words and source data", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0xff007986, 7, 0x0c10a904, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::STG);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.STG(lowered.instructions.front());
    std::vector<u32> registers;
    u32 stores{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister)
            registers.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::CompositeConstructU32x2) {
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg::R220);
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg::R221);
        }
        if (inst.GetOpcode() == IR::Opcode::WriteGlobal32) {
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg::R7);
            ++stores;
        }
    }
    std::sort(registers.begin(), registers.end());
    REQUIRE(registers == std::vector<u32>{7, 220, 221});
    REQUIRE(stores == 1);
    input[0].words[2] |= 1; // An unaligned uniform address pair is unsupported.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    input[0].words[2] = 0x0c10ab04; // A different store width must not become scalar.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
}

TEST_CASE("Captured native compute compiles with shared memory and atomics", "[.native-compute-captured]") {
    const char* directory = std::getenv("EDEN_NATIVE_SHADER_FIXTURES");
    REQUIRE(directory != nullptr);
    std::ifstream file(std::filesystem::path{directory} / "compute.source.bin", std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const auto size = static_cast<size_t>(file.tellg());
    REQUIRE(size % 16 == 0);
    std::vector<Native> input(size / 16);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(input.data()), size);
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    INFO("offset=" << lowered.error_offset << " error=" << lowered.error);
    REQUIRE(lowered);
    Shader::Ampere::ProgramCaptureState capture;
    u32 captured_size{};
    for (u32 i = 0; i < input.size(); ++i) {
        const auto status = capture.Observe(input[i], i);
        REQUIRE(status != Shader::Ampere::ProgramCaptureState::Result::InvalidTarget);
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            captured_size = (i + 1) * sizeof(Native);
            break;
        }
    }
    REQUIRE(captured_size == lowered.source_size);
    std::vector<u64> code(0x50 / 8);
    for (u64 word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    std::array<u32, 64> qmd{};
    std::ifstream descriptor(std::filesystem::path{directory} / "compute.qmd.bin", std::ios::binary);
    REQUIRE(descriptor.good());
    descriptor.read(reinterpret_cast<char*>(qmd.data()), sizeof(qmd));
    const std::array<u32, 3> group{qmd[18] >> 16, qmd[19] & 0xffff, qmd[19] >> 16};
    const u32 shared_size = qmd[17] & 0x3ffff;
    class ComputeFixtureEnvironment final : public EmptyEnvironment {
    public:
        ComputeFixtureEnvironment(const std::vector<u64>& code_, u32 shared_, std::array<u32, 3> group_, Shader::TextureType texture_)
            : code{code_}, shared{shared_}, group{group_}, texture{texture_} {
            stage = Shader::Stage::Compute;
            start_address = 0x50;
            is_proprietary_driver = true;
        }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        u32 TranslateProgramAddress(u32 address) const override {
            const auto translated = Shader::Ampere::NativeProgramAddress(code, address);
            if (!translated) throw std::logic_error("invalid compute branch target");
            return *translated;
        }
        u32 ReadCbufValue(u32 slot, u32 offset) override {
            std::ifstream file(std::filesystem::path{std::getenv("EDEN_NATIVE_SHADER_FIXTURES")} /
                               ("compute.cbuf" + std::to_string(slot) + ".bin"), std::ios::binary);
            if (!file) return 0;
            file.seekg(offset);
            u32 value{};
            file.read(reinterpret_cast<char*>(&value), sizeof(value));
            if (!file) throw std::logic_error("captured compute constant outside buffer");
            return value;
        }
        u32 TextureBoundBuffer() const override { return 0; }
        u32 SharedMemorySize() const override { return shared; }
        std::array<u32, 3> WorkgroupSize() const override { return group; }
        Shader::TextureType ReadTextureType(u32) override { return texture; }
        const std::vector<u64>& code;
        u32 shared;
        std::array<u32, 3> group;
        Shader::TextureType texture;
    } env{code, shared_size, group,
        std::any_of(input.begin(), input.end(), [](const auto& instruction) {
            return instruction.Bits(0, 12) == 0xb66 && instruction.Bits(61, 3) == 2;
        }) ? Shader::TextureType::Color3D : std::any_of(input.begin(), input.end(), [](const auto& instruction) {
            return (instruction.Bits(0, 12) == 0xb66 && instruction.Bits(61, 3) == 1) ||
                   instruction.Bits(0, 12) == 0xb97 || instruction.Bits(0, 12) == 0xb9b;
        }) ? Shader::TextureType::Color2D : Shader::TextureType::Color1D};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, env.StartAddress()};
    Shader::HostTranslateInfo host;
    host.support_int64 = true;
    host.support_float64 = true;
    host.support_float16 = true;
    host.support_conditional_barrier = true;
    const auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    std::ofstream(std::filesystem::path{directory} / "compute.ir.txt") << IR::DumpProgram(program);
    REQUIRE(program.stage == Shader::Stage::Compute);
    REQUIRE_FALSE(program.blocks.empty());
    u32 barriers{}, shared_atomics{}, global_atomics{}, stores{}, image_stores{}, image_loads{};
    for (const auto* block : program.blocks) for (const auto& inst : block->Instructions()) {
        image_loads += inst.GetOpcode() == IR::Opcode::ImageRead || inst.GetOpcode() == IR::Opcode::BoundImageRead;
        barriers += inst.GetOpcode() == IR::Opcode::Barrier;
        shared_atomics += inst.GetOpcode() == IR::Opcode::SharedAtomicIAdd32;
        global_atomics += inst.GetOpcode() == IR::Opcode::GlobalAtomicIAdd32 || inst.GetOpcode() == IR::Opcode::StorageAtomicIAdd32;
        stores += inst.GetOpcode() == IR::Opcode::WriteGlobal32 || inst.GetOpcode() == IR::Opcode::WriteStorage32;
        image_stores += inst.GetOpcode() == IR::Opcode::ImageWrite || inst.GetOpcode() == IR::Opcode::BoundImageWrite;
    }
    const bool surface = std::any_of(input.begin(), input.end(), [](const auto& instruction) { return instruction.Bits(0, 12) == 0xb9b; });
    if (surface) {
        const auto expected_barriers = std::count_if(input.begin(), input.end(), [](const auto& instruction) {
            return instruction.Bits(0, 12) == 0xb1d;
        });
        REQUIRE(barriers >= expected_barriers);
        REQUIRE(image_stores >= 1);
        const auto expected_loads = std::count_if(input.begin(), input.end(), [](const auto& instruction) {
            return instruction.Bits(0, 12) == 0xb97;
        });
        REQUIRE(image_loads >= expected_loads);
    } else if (std::any_of(input.begin(), input.end(), [](const auto& instruction) { return instruction.Bits(0, 12) == 0xf8c; })) {
        REQUIRE(barriers >= 2);
        REQUIRE(shared_atomics >= 4);
        REQUIRE(global_atomics >= 2);
    } else if (shared_size && std::any_of(input.begin(), input.end(), [](const auto& instruction) {
        return instruction.Bits(0, 12) == 0x38c;
    })) {
        REQUIRE(barriers >= 5);
        REQUIRE(shared_atomics >= 2);
        REQUIRE(global_atomics >= 1);
        REQUIRE(stores >= 8);
    } else {
        const auto expected_barriers = std::count_if(input.begin(), input.end(), [](const auto& instruction) {
            return instruction.Bits(0, 12) == 0xb1d;
        });
        if (expected_barriers) REQUIRE(barriers >= expected_barriers);
        else REQUIRE(barriers == 0);
        REQUIRE(stores >= 4);
    }
}

namespace {
double DecodeTestHalf(u16 bits) {
    const u32 exponent = (bits >> 10) & 31, mantissa = bits & 1023;
    const double value = exponent == 31 ? (mantissa ? std::numeric_limits<double>::quiet_NaN()
                                                   : std::numeric_limits<double>::infinity())
                        : exponent == 0 ? std::ldexp(static_cast<double>(mantissa), -24)
                                        : std::ldexp(static_cast<double>(1024 + mantissa), static_cast<int>(exponent) - 25);
    return (bits & 0x8000) ? -value : value;
}
u16 EncodeTestHalf(double value) {
    const u16 sign = std::signbit(value) ? 0x8000 : 0;
    value = std::abs(value);
    if (std::isnan(value)) return 0xfe01; // Deliberately noncanonical: ReLU must canonicalize it.
    if (value >= 65520) return sign | 0x7c00;
    if (value < std::ldexp(1.0, -14))
        return sign | static_cast<u16>(std::nearbyint(std::ldexp(value, 24)));
    int exponent{};
    std::frexp(value, &exponent);
    --exponent;
    u32 mantissa = static_cast<u32>(std::nearbyint(std::ldexp(value, 10 - exponent)));
    if (mantissa == 2048) { ++exponent; mantissa = 1024; }
    return sign | static_cast<u16>(((exponent + 15) << 10) | (mantissa - 1024));
}
struct ScalarNativeMachine {
    std::array<u32, 256> regs{};
    std::array<bool, 7> predicates{};
    std::unordered_map<u64, u32> constants, global, shared;
    u32 workgroup_barriers{}, device_barriers{};
    void Execute(IR::Block& block) {
        std::unordered_map<const IR::Inst*, u64> values;
        bool carry{}, zero{}, sign{}, overflow{};
        const auto get = [&](const IR::Value& value) -> u64 {
            if (!value.IsImmediate()) return values.at(value.Inst());
            if (value.Type() == IR::Type::U1) return value.U1();
            if (value.Type() == IR::Type::F16 || value.Type() == IR::Type::U16) return value.U16();
            if (value.Type() == IR::Type::U64) return value.U64();
            if (value.Type() == IR::Type::F32) return std::bit_cast<u32>(value.F32());
            return value.U32();
        };
        for (const auto& inst : block.Instructions()) {
            const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
            u64 result{};
            switch (inst.GetOpcode()) {
            case IR::Opcode::BitCastF32U32: case IR::Opcode::BitCastU32F32: result = arg(0); break;
            case IR::Opcode::UnpackFloat2x16: result = (arg(0) & 0xffff) | ((arg(0) >> 16) << 32); break;
            case IR::Opcode::PackFloat2x16: result = (arg(0) & 0xffff) | ((arg(0) >> 32) << 16); break;
            case IR::Opcode::CompositeConstructF16x2: result = arg(0) | (arg(1) << 32); break;
            case IR::Opcode::CompositeExtractF16x2: result = (arg(0) >> (arg(1) * 32)) & 0xffff; break;
            case IR::Opcode::FPNeg16: result = arg(0) ^ 0x8000; break;
            case IR::Opcode::FPAbs16: result = arg(0) & 0x7fff; break;
            case IR::Opcode::FPFma32: result = std::bit_cast<u32>(std::fma(std::bit_cast<float>(static_cast<u32>(arg(0))),
                std::bit_cast<float>(static_cast<u32>(arg(1))), std::bit_cast<float>(static_cast<u32>(arg(2))))); break;
            case IR::Opcode::FPSaturate32: {
                const float value = std::bit_cast<float>(static_cast<u32>(arg(0)));
                result = std::bit_cast<u32>(std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f));
                break;
            }
            case IR::Opcode::FPFma16: result = EncodeTestHalf(std::fma(DecodeTestHalf(static_cast<u16>(arg(0))),
                DecodeTestHalf(static_cast<u16>(arg(1))), DecodeTestHalf(static_cast<u16>(arg(2))))); break;
            case IR::Opcode::FPFloor32: result = std::bit_cast<u32>(std::floor(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::ConvertF32F16: result = std::bit_cast<u32>(static_cast<float>(DecodeTestHalf(static_cast<u16>(arg(0))))); break;
            case IR::Opcode::FPOrdEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) == std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPOrdLessThanEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) <= std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPOrdGreaterThanEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) >= std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPMax32: result = std::bit_cast<u32>(std::fmax(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
            case IR::Opcode::FPMin32: result = std::bit_cast<u32>(std::fmin(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
            case IR::Opcode::ConvertF32U32: result = std::bit_cast<u32>(static_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::ConvertU32F32: result = static_cast<u32>(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPTrunc32: result = std::bit_cast<u32>(std::trunc(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::FPNeg32: result = arg(0) ^ 0x80000000U; break;
            case IR::Opcode::IAbs32: result = static_cast<s32>(arg(0)) < 0 ? 0U - static_cast<u32>(arg(0)) : static_cast<u32>(arg(0)); break;
            case IR::Opcode::FPAbs32: result = arg(0) & 0x7fffffffU; break;
            case IR::Opcode::ConvertF16F32: result = EncodeTestHalf(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPRoundEven32: result = std::bit_cast<u32>(std::nearbyint(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::FPIsNan32: result = std::isnan(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPClamp32: result = std::bit_cast<u32>(std::clamp(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))), std::bit_cast<float>(static_cast<u32>(arg(2))))); break;
            case IR::Opcode::ConvertS32F32: {
                const float value = std::bit_cast<float>(static_cast<u32>(arg(0)));
                result = std::isnan(value) ? 0x87654321U : static_cast<u32>(static_cast<s32>(value));
                break;
            }
            case IR::Opcode::GetRegister: result = inst.Arg(0).Reg() == IR::Reg::RZ ? 0 : regs[static_cast<u32>(inst.Arg(0).Reg())]; break;
            case IR::Opcode::SetRegister: if (inst.Arg(0).Reg() != IR::Reg::RZ) regs[static_cast<u32>(inst.Arg(0).Reg())] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::GetPred: result = predicates[static_cast<u32>(inst.Arg(0).Pred())]; break;
            case IR::Opcode::SetPred: predicates[static_cast<u32>(inst.Arg(0).Pred())] = arg(1) != 0; break;
            case IR::Opcode::IAdd32: result = static_cast<u32>(arg(0) + arg(1)); break;
            case IR::Opcode::ISub32: result = static_cast<u32>(arg(0) - arg(1)); break;
            case IR::Opcode::IMul32: result = static_cast<u32>(arg(0) * arg(1)); break;
            case IR::Opcode::IAdd64: result = arg(0) + arg(1); break;
            case IR::Opcode::UMax32: result = std::max(arg(0), arg(1)); break;
            case IR::Opcode::UMin32: result = std::min(arg(0), arg(1)); break;
            case IR::Opcode::FindUMsb32: result = arg(0) ? 31U - std::countl_zero(static_cast<u32>(arg(0))) : ~0U; break;
            case IR::Opcode::BitReverse32: {
                const u32 source = static_cast<u32>(arg(0));
                for (u32 bit = 0; bit < 32; ++bit) result |= u64{(source >> bit) & 1U} << (31 - bit);
                break;
            }
            case IR::Opcode::ShiftLeftLogical64: result = arg(1) < 64 ? arg(0) << arg(1) : 0; break;
            case IR::Opcode::ShiftRightLogical64: result = arg(1) < 64 ? arg(0) >> arg(1) : 0; break;
            case IR::Opcode::INeg32: result = 0U - static_cast<u32>(arg(0)); break;
            case IR::Opcode::ShiftLeftLogical32: result = arg(1) < 32 ? static_cast<u32>(arg(0) << arg(1)) : 0; break;
            case IR::Opcode::ShiftRightArithmetic32: result = static_cast<u32>(static_cast<s32>(arg(0)) >> std::min<u64>(arg(1), 31)); break;
            case IR::Opcode::ShiftRightLogical32: result = arg(1) < 32 ? static_cast<u32>(arg(0)) >> arg(1) : 0; break;
            case IR::Opcode::BitwiseAnd32: result = arg(0) & arg(1); break;
            case IR::Opcode::BitwiseOr32: result = arg(0) | arg(1); break;
            case IR::Opcode::BitwiseXor32: result = arg(0) ^ arg(1); break;
            case IR::Opcode::BitwiseNot32: result = arg(0) ^ 0xffffffffULL; break;
            case IR::Opcode::BitFieldSExtract: {
                const u32 value = static_cast<u32>(arg(0)) >> arg(1);
                const u32 width = static_cast<u32>(arg(2));
                result = width == 0 ? 0 : width == 32 ? value :
                    static_cast<u32>(static_cast<s32>(value << (32 - width)) >> (32 - width));
                break;
            }
            case IR::Opcode::BitFieldUExtract: result = (arg(0) >> arg(1)) & ((u64{1} << arg(2)) - 1); break;
            case IR::Opcode::BitFieldInsert: { const u64 mask = ((u64{1} << arg(3)) - 1) << arg(2); result = (arg(0) & ~mask) | ((arg(1) << arg(2)) & mask); break; }
            case IR::Opcode::SelectF32: case IR::Opcode::SelectU32: case IR::Opcode::SelectU1: result = arg(0) ? arg(1) : arg(2); break;
            case IR::Opcode::LogicalAnd: result = arg(0) && arg(1); break;
            case IR::Opcode::LogicalOr: result = arg(0) || arg(1); break;
            case IR::Opcode::LogicalXor: result = (arg(0) != 0) != (arg(1) != 0); break;
            case IR::Opcode::LogicalNot: result = !arg(0); break;
            case IR::Opcode::SMin32: result = static_cast<u32>(std::min(static_cast<s32>(arg(0)), static_cast<s32>(arg(1)))); break;
            case IR::Opcode::SMax32: result = static_cast<u32>(std::max(static_cast<s32>(arg(0)), static_cast<s32>(arg(1)))); break;
            case IR::Opcode::SLessThan: result = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1)); break;
            case IR::Opcode::ULessThan: result = arg(0) < arg(1); break;
            case IR::Opcode::IEqual: result = arg(0) == arg(1); break;
            case IR::Opcode::INotEqual: result = arg(0) != arg(1); break;
            case IR::Opcode::GetCFlag: result = carry; break;
            case IR::Opcode::SetCFlag: carry = arg(0) != 0; break;
            case IR::Opcode::GetZFlag: result = zero; break;
            case IR::Opcode::SetZFlag: zero = arg(0) != 0; break;
            case IR::Opcode::GetSFlag: result = sign; break;
            case IR::Opcode::SetSFlag: sign = arg(0) != 0; break;
            case IR::Opcode::GetOFlag: result = overflow; break;
            case IR::Opcode::SetOFlag: overflow = arg(0) != 0; break;
            case IR::Opcode::GetCarryFromOp: { const auto* sum = inst.Arg(0).Inst(); result = get(sum->Arg(0)) + get(sum->Arg(1)) > 0xffffffffULL; break; }
            case IR::Opcode::GetZeroFromOp: result = arg(0) == 0; break;
            case IR::Opcode::GetSignFromOp: result = (arg(0) & 0x80000000U) != 0; break;
            case IR::Opcode::GetOverflowFromOp: { const auto* sum = inst.Arg(0).Inst(); result = ((get(sum->Arg(0)) ^ arg(0)) & (get(sum->Arg(1)) ^ arg(0)) & 0x80000000U) != 0; break; }
            case IR::Opcode::GetCbufU32: result = constants.at((arg(0) << 32) | arg(1)); break;
            case IR::Opcode::CompositeConstructU32x2: result = arg(0) | (arg(1) << 32); break;
            case IR::Opcode::PackUint2x32: case IR::Opcode::UnpackUint2x32: result = arg(0); break;
            case IR::Opcode::CompositeExtractU32x2: result = static_cast<u32>(arg(0) >> (arg(1) * 32)); break;
            case IR::Opcode::WriteSharedU64: shared[arg(0)] = static_cast<u32>(arg(1)); shared[arg(0) + 4] = static_cast<u32>(arg(1) >> 32); break;
            case IR::Opcode::LoadSharedU64: result = shared.at(arg(0)) | (u64{shared.at(arg(0) + 4)} << 32); break;
            case IR::Opcode::LoadGlobal32: result = global.at(arg(0)); break;
            case IR::Opcode::WriteGlobal32: global[arg(0)] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::WriteSharedU32: shared[arg(0)] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::LoadSharedU32: result = shared.at(arg(0)); break;
            case IR::Opcode::GlobalAtomicIAdd32: result = global.at(arg(0)); global[arg(0)] += static_cast<u32>(arg(1)); break;
            case IR::Opcode::SharedAtomicUMin32: result = shared.at(arg(0)); shared[arg(0)] = std::min(shared[arg(0)], static_cast<u32>(arg(1))); break;
            case IR::Opcode::SharedAtomicUMax32: result = shared.at(arg(0)); shared[arg(0)] = std::max(shared[arg(0)], static_cast<u32>(arg(1))); break;
            case IR::Opcode::SharedAtomicOr32: result = shared.at(arg(0)); shared[arg(0)] |= static_cast<u32>(arg(1)); break;
            case IR::Opcode::SharedAtomicIAdd32: result = shared.at(arg(0)); shared[arg(0)] += static_cast<u32>(arg(1)); break;
            case IR::Opcode::Barrier: ++workgroup_barriers; break;
            case IR::Opcode::DeviceMemoryBarrier: ++device_barriers; break;
            default: FAIL("Unexpected scalar compute IR " << IR::NameOf(inst.GetOpcode()));
            }
            values[&inst] = result;
        }
    }
};
void TranslateScalarNative(std::span<const Native> input, IR::Block& block) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    INFO("offset=" << lowered.error_offset << " error=" << lowered.error);
    REQUIRE(lowered);
    EmptyEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (u64 word : lowered.instructions) {
        const auto opcode = Shader::Maxwell::Decode(word);
        if (opcode == Shader::Maxwell::Opcode::EXIT) continue;
        switch (opcode) {
#define INST(name, cute, encode) case Shader::Maxwell::Opcode::name: visitor.name(word); break;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef INST
        }
    }
}
}

TEST_CASE("Native mixed-bank texel fetches retain descriptor coordinates and split channels", "[native-shader]") {
    const u32 dimension = GENERATE(0U, 1U, 2U, 5U);
    const u32 mask = GENERATE(1U, 3U, 7U, 15U);
    const u32 second = mask >= 7 ? 12U : 255U;
    const std::array<Native, 3> input{{
        {{0x03027b6f, 0x01c00800, 0x000003ff, 0}},
        {{0x04107b66, (dimension << 29) | 0x10c00808U, 0x049e0000U | (mask << 8) | second, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 7);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 handles{}, fetches{};
    std::vector<std::pair<u32, u32>> copies;
    std::vector<u32> channels;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetCbufU32) {
            REQUIRE(inst.Arg(0).U32() == 3);
            REQUIRE(inst.Arg(1).U32() == 32);
            ++handles;
        }
        if (inst.GetOpcode() == IR::Opcode::BindlessImageFetch) {
            ++fetches;
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{253});
            const auto type = dimension == 0 ? Shader::TextureType::Color1D :
                dimension == 1 ? Shader::TextureType::Color2D :
                dimension == 2 ? Shader::TextureType::Color3D : Shader::TextureType::ColorArray2D;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == type);
            REQUIRE(inst.Arg(3).U32() == 0);
        }
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4) channels.push_back(inst.Arg(1).U32());
        if (inst.GetOpcode() == IR::Opcode::SetRegister && !inst.Arg(1).IsImmediate() &&
            inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
    }
    REQUIRE(handles == 1);
    REQUIRE(fetches == 1);
    REQUIRE(copies.front() == std::pair<u32, u32>{250, 4});
    if (dimension == 1) REQUIRE(copies[1] == std::pair<u32, u32>{251, 8});
    if (dimension == 2 || dimension == 5) {
        REQUIRE(copies[1] == std::pair<u32, u32>{251, 5});
        REQUIRE(copies[2] == std::pair<u32, u32>{252, 8});
    }
    std::vector<u32> expected_channels;
    for (u32 i = 0; i < 4; ++i) if (mask & (1U << i)) expected_channels.push_back(i);
    REQUIRE(channels == expected_channels);
    for (u32 i = 0; i < expected_channels.size(); ++i)
        REQUIRE(copies[copies.size() - expected_channels.size() + i] ==
                std::pair<u32, u32>{i < 2 ? 16U + i : 12U + i - 2, 250U + i});
}

TEST_CASE("Native mixed-bank samples preserve 2D and cube sampling modes", "[native-shader]") {
    const bool cube = GENERATE(false, true);
    const bool explicit_lod = GENERATE(false, true);
    const bool rgba = GENERATE(false, true);
    const u32 mask = rgba ? 15U : 3U;
    const std::array<Native, 3> input{{
        {{0x03027b6f, 0x01c00800, 0x000003ff, 0}},
        {{0x04107b60, (cube ? 0x70c00808U : 0x30c00808U),
          0x041e0000U | ((cube ? explicit_lod ? 3U : 1U : explicit_lod ? 1U : 0U) << 23) | (mask << 8) | (rgba ? 12U : 255U), 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 samples{}, handles{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetCbufU32) {
            REQUIRE(inst.Arg(0).U32() == 3);
            REQUIRE(inst.Arg(1).U32() == 32);
            ++handles;
        }
        if (inst.GetOpcode() == IR::Opcode::BindlessImageSampleExplicitLod ||
            inst.GetOpcode() == IR::Opcode::BindlessImageSampleImplicitLod) {
            REQUIRE(inst.GetOpcode() == (cube || explicit_lod ? IR::Opcode::BindlessImageSampleExplicitLod : IR::Opcode::BindlessImageSampleImplicitLod));
            ++samples;
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{cube ? 250U : 253U});
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() ==
                    (cube ? Shader::TextureType::ColorCube : Shader::TextureType::Color2D));
            if (cube && explicit_lod)
                REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{251});
            else if (cube || explicit_lod) REQUIRE(inst.Arg(2).F32() == 0.0f);
        }
    }
    REQUIRE(samples == 1);
    REQUIRE(handles == 1);
}

TEST_CASE("Native CTA-strong global loads retain memory ordering around the read", "[native-shader]") {
    const std::array<Native, 2> input{{{{0xff247981, 4, 0x081ead00, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 barriers{}, loads{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::DeviceMemoryBarrier) {
            REQUIRE(loads == barriers);
            ++barriers;
        }
        if (inst.GetOpcode() == IR::Opcode::LoadGlobal128) {
            REQUIRE(barriers == 1);
            ++loads;
        }
    }
    REQUIRE(loads == 1);
    REQUIRE(barriers == 2);
}

TEST_CASE("Native C8 bitwise LUT preserves each bit and predicate output", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00037212, 1, 0x0782c802, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 a : {0U, 1U, 0xaaaaaaaaU, 0xffffffffU})
        for (u32 b : {0U, 1U, 0x55555555U, 0xffffffffU})
            for (u32 c : {0U, 1U, 0xccccccccU, 0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[0] = a; machine.regs[1] = b; machine.regs[2] = c;
                machine.predicates = {true, false, true, false, true, false, true};
                auto expected = machine.predicates;
                expected[1] = ((a | c) & b) != 0;
                machine.Execute(block);
                REQUIRE(machine.regs[3] == ((a | c) & b));
                REQUIRE(machine.predicates == expected);
            }
}

TEST_CASE("Native extended equality comparisons retain the low-word predicate", "[native-shader]") {
    const bool equal = GENERATE(false, true);
    const u32 comparison = equal ? 2U : 5U;
    const std::array<Native, 3> input{{
        {{0x0000720c, 2, 0x03f00000U | (comparison << 12), 0}},
        {{0x0100720c, 3, 0x03f00100U | (comparison << 12), 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u64 a : {0ULL, 1ULL, 0xffffffffULL, 0x100000000ULL, 0xffffffffffffffffULL})
        for (u64 b : {0ULL, 1ULL, 0xffffffffULL, 0x100000000ULL, 0xffffffffffffffffULL}) {
            ScalarNativeMachine machine;
            machine.regs[0] = static_cast<u32>(a); machine.regs[1] = static_cast<u32>(a >> 32);
            machine.regs[2] = static_cast<u32>(b); machine.regs[3] = static_cast<u32>(b >> 32);
            machine.predicates = {false, true, false, true, false, true, false};
            auto expected = machine.predicates;
            expected[0] = equal ? a == b : a != b;
            machine.Execute(block);
            REQUIRE(machine.predicates == expected);
            REQUIRE(machine.regs[0] == static_cast<u32>(a));
            REQUIRE(machine.regs[1] == static_cast<u32>(a >> 32));
        }
    auto invalid = input;
    invalid[1].words[2] ^= 1U << 23;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native bindless dimension queries keep handle mip and overlapping destinations", "[native-shader]") {
    const u32 mask = GENERATE(1U, 3U, 8U);
    const std::array<Native, 2> input{{{{0x1c1d7370, 0x08000000, 0xffU | (mask << 8), 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 queries{};
    std::vector<u32> writes, channels;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BindlessImageQueryDimensions) {
            ++queries;
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{28});
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg{29});
            REQUIRE(inst.Arg(2).U1() == (mask != 8));
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
            channels.push_back(inst.Arg(1).Inst()->Arg(1).U32());
        }
    }
    REQUIRE(queries == 1);
    REQUIRE(writes == (mask == 3 ? std::vector<u32>{29, 30} : std::vector<u32>{29}));
    REQUIRE(channels == (mask == 3 ? std::vector<u32>{0, 1} : mask == 8 ? std::vector<u32>{3} : std::vector<u32>{0}));
}

TEST_CASE("Native scalar bindless cube sampling keeps XYZ and the trailing handle", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x28707361, 0x7800002a, 0x041e01ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    std::vector<std::pair<u32, u32>> copies;
    u32 samples{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BindlessImageSampleImplicitLod) {
            ++samples;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorCube);
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250});
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->GetOpcode() == IR::Opcode::CompositeConstructF32x3);
            for (u32 i = 0; i < 3; ++i)
                REQUIRE(coords->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{252 + i});
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister && inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
    }
    REQUIRE(samples == 1);
    REQUIRE(copies == std::vector<std::pair<u32, u32>>{{252, 40}, {253, 41}, {254, 42}, {250, 43}, {112, 250}});
}

TEST_CASE("Native scalar bindless 2D sampling retains packed offsets and handle aliases", "[native-shader]") {
    const bool offset = GENERATE(false, true);
    const std::array<Native, 2> input{{{{0x1c1e7361, 0x3800001e, 0x041e01ffU | (u32{offset} << 12), 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 samples{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BindlessImageSampleImplicitLod) {
            ++samples;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color2D);
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{30});
            const auto* coords = inst.Arg(1).Inst();
            for (u32 i = 0; i < 2; ++i)
                REQUIRE(coords->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{28 + i});
            if (offset) {
                const auto* offsets = inst.Arg(3).Inst();
                REQUIRE(offsets->GetOpcode() == IR::Opcode::CompositeConstructU32x2);
                for (u32 i = 0; i < 2; ++i) {
                    const auto* field = offsets->Arg(i).Inst();
                    REQUIRE(field->GetOpcode() == IR::Opcode::BitFieldSExtract);
                    REQUIRE(field->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{31});
                    REQUIRE(field->Arg(1).U32() == i * 4);
                    REQUIRE(field->Arg(2).U32() == 4);
                }
            } else REQUIRE(inst.Arg(3).IsEmpty());
        }
    }
    REQUIRE(samples == 1);
}

TEST_CASE("Native bindless cube sampling retains packed channels and explicit LOD", "[native-shader]") {
    const u32 form = GENERATE(0U, 1U, 2U, 3U, 4U);
    const u32 mask = GENERATE(1U, 3U, 7U, 15U);
    const bool scalar = form < 2;
    const u32 lod = form == 2 ? 3U : form == 1 || form == 4 ? 1U : 0U;
    const u32 second = mask == 7 ? 19U : mask == 15 ? 18U : 255U;
    const std::array<Native, 2> input{{
        {{0x04107361, (scalar ? 0x78000008U : 0x68000008U),
          0x041e0000U | (lod << 23) | (mask << 8) | second, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    std::vector<std::pair<u32, u32>> copies;
    std::vector<u32> channels;
    u32 samples{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BindlessImageSampleImplicitLod ||
            inst.GetOpcode() == IR::Opcode::BindlessImageSampleExplicitLod) {
            ++samples;
            REQUIRE(inst.GetOpcode() == (lod ? IR::Opcode::BindlessImageSampleExplicitLod : IR::Opcode::BindlessImageSampleImplicitLod));
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorCube);
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250});
            if (lod == 3) REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{251});
            else if (lod == 1) REQUIRE(inst.Arg(2).F32() == 0.0f);
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister && inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractF32x4) channels.push_back(inst.Arg(1).U32());
    }
    REQUIRE(samples == 1);
    REQUIRE(copies[0] == std::pair<u32, u32>{252, 4});
    REQUIRE(copies[1] == std::pair<u32, u32>{253, 5});
    REQUIRE(copies[2] == std::pair<u32, u32>{254, scalar ? 8U : 6U});
    REQUIRE(copies[3] == std::pair<u32, u32>{250, scalar ? 9U : 8U});
    if (lod == 3) REQUIRE(copies[4] == std::pair<u32, u32>{251, 9});
    std::vector<u32> expected;
    for (u32 i = 0; i < 4; ++i) if (mask & (1U << i)) expected.push_back(i);
    REQUIRE(channels == expected);
    for (u32 i = 0; i < expected.size(); ++i)
        REQUIRE(copies[copies.size() - expected.size() + i] ==
                std::pair<u32, u32>{i < 2 ? 16U + i : second + i - 2, 250U + i});
}

TEST_CASE("Native predicated loop exits retain cleanup and the active reconvergence", "[native-shader]") {
    const std::array<Native, 11> input{{
        {{0x27945, 144, 0x03800000, 0}},
        {{0x00007202, 1, 0xf00, 0}},
        {{0x8944, 16, 0x03c00000, 0}},
        {{0x7947, 0xffffffd0, 0x0383ffff, 0}},
        {{0x00057202, 0, 0xf00, 0}},
        {{0x7947, 48, 0x03800000, 0}},
        {{0x00057202, 1, 0xf00, 0}},
        {{0x00057202, 1, 0xf00, 0}},
        {{0x00057202, 1, 0xf00, 0}},
        {{0x27941, 0, 0x03800000, 0}}, VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto invalid = input;
    invalid[5].words[1] = 0xffffffe0;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[3].words[1] = 0xffffffc0;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[4].words[0] = 0x00057802;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    std::vector<u64> code(0x50 / 8);
    for (const u64 word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class LoopEnvironment final : public EmptyEnvironment {
    public:
        explicit LoopEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const auto location = [&](u32 index) {
        const u32 offset = lowered.source_instruction_offsets[index];
        return Shader::Maxwell::Location{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    };
    for (bool initial : {false, true}) {
        bool predicate = initial, cleaned{}, joined{}, exited{}, repeated{};
        const auto* block = &*cfg.Functions().front().blocks.begin();
        for (u32 step = 0; step < 32 && block; ++step) {
            if (block->begin == location(3)) { repeated = true; predicate = false; }
            cleaned |= block->Contains(location(4));
            if (block->begin == location(9)) {
                joined = true;
                REQUIRE(block->stack.Peek(Shader::Maxwell::Flow::Token::SSY).has_value());
                REQUIRE(block->branch_true != nullptr);
                REQUIRE(block->branch_true->begin == location(10));
                REQUIRE_FALSE(block->branch_true->stack.Peek(Shader::Maxwell::Flow::Token::SSY).has_value());
            }
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            block = ((pred == IR::Pred::PT || predicate) != inverted) ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        REQUIRE(cleaned);
        REQUIRE(joined);
        REQUIRE(repeated == initial);
    }
}

TEST_CASE("Native shared atomic extrema retain unsigned values and optional old results", "[native-shader]") {
    const bool minimum = GENERATE(false, true);
    const bool discard = GENERATE(false, true);
    const std::array<Native, 2> input{{{{discard ? 0xffff738cU : 0xff05738cU, 0x801,
        minimum ? 0x00800000U : 0x01000000U, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 old : {0U, 1U, 0x80000000U, 0xffffffffU})
        for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.shared[8] = old;
            machine.regs[1] = value;
            machine.regs[5] = 0x12345678;
            machine.Execute(block);
            REQUIRE(machine.shared[8] == (minimum ? std::min(old, value) : std::max(old, value)));
            REQUIRE(machine.regs[1] == value);
            REQUIRE(machine.regs[5] == (discard ? 0x12345678U : old));
        }
    auto guarded = input;
    guarded[0].words[0] = (guarded[0].words[0] & ~0xf000U) | 0x3000;
    const auto lowered = Shader::Ampere::LowerToMaxwell(guarded);
    REQUIRE(lowered);
    REQUIRE(Shader::Maxwell::Instruction{lowered.instructions.front()}.Pred().index == 3);
}

TEST_CASE("Texture handle tracking cannot substitute a buffer address for loaded data", "[native-shader]") {
    const u32 mode = GENERATE(0U, 1U, 2U, 3U, 4U);
    class HandleEnvironment final : public EmptyEnvironment {
    public:
        u32 reads{};
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            ++reads;
            REQUIRE(bank == 0);
            REQUIRE(offset == 0x510);
            return 7;
        }
        Shader::TextureType ReadTextureType(u32 handle) override {
            REQUIRE(handle == 7);
            return Shader::TextureType::Color2D;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 pointer{ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U))};
    IR::U32 handle = pointer;
    const auto storage_load = [&](IR::Opcode opcode) {
        const auto it = block.PrependNewInst(block.end(), opcode, {ir.Imm32(0U), pointer});
        return IR::Value{&*it};
    };
    if (mode == 1 || mode == 4) handle = IR::U32{storage_load(IR::Opcode::LoadStorage32)};
    if (mode == 2) handle = IR::U32{ir.CompositeExtract(storage_load(IR::Opcode::LoadStorage64), 0)};
    if (mode == 3) {
        const IR::U64 address{ir.PackUint2x32(ir.CompositeConstruct(pointer, ir.Imm32(0x19U)))};
        handle = ir.LoadGlobal32(address);
    }
    if (mode == 4) handle = ir.BitwiseOr(handle, pointer);
    IR::TextureInstInfo info{};
    info.type.Assign(Shader::TextureType::Color2D);
    (void)ir.ImageSampleImplicitLod(handle, ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)), {}, {}, {}, info);
    IR::Program program;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Shader::HostTranslateInfo host;
    if (mode == 0) {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        REQUIRE(program.info.texture_descriptors.size() == 1);
        REQUIRE(program.info.texture_descriptors[0].cbuf_index == 0);
        REQUIRE(program.info.texture_descriptors[0].cbuf_offset == 0x510);
        REQUIRE(env.reads == 1);
    } else {
        REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
        REQUIRE(env.reads == 0);
    }
}

TEST_CASE("Buffer-backed texture arrays retain record indices and root descriptor bounds", "[native-shader]") {
    const bool decomposed = GENERATE(false, true);
    const bool constant_index = GENERATE(false, true);
    const bool query = GENERATE(false, true);
    const u32 failure = GENERATE(0U, 1U, 2U, 3U, 4U);
    class BufferEnvironment final : public EmptyEnvironment {
    public:
        u32 size{40960};
        u32 high{};
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return size;
            REQUIRE(offset == 0x51c);
            return high;
        }
        Shader::TextureType ReadTextureType(u32) override {
            FAIL("A buffer address must not be resolved as a texture handle");
            return {};
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 index = constant_index ? ir.Imm32(4U) : ir.GetReg(IR::Reg::R1);
    const auto constant = [&](u32 value) { return ir.Imm32(value); };
    IR::U32 product = ir.IMul(index, constant(160));
    if (decomposed) {
        const auto low = ir.BitFieldExtract(index, constant(0), constant(16));
        const auto high = ir.BitFieldExtract(index, constant(16), constant(16));
        product = ir.IAdd(ir.ShiftLeftLogical(ir.IAdd(ir.IMul(low, constant(0)), ir.IMul(high, constant(160))), constant(16)),
                         ir.IMul(low, constant(160)));
    }
    const IR::U32 pointer{ir.CompositeExtract(ir.GetCbuf(constant(0), constant(0x510), 64, false), 0)};
    const auto relative = ir.IAdd(product, constant(152));
    const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(ir.GetCbuf(constant(0), constant(0x510)), constant(0xfffffff0)));
    const auto load = block.PrependNewInst(block.end(), IR::Opcode::LoadStorage64, {constant(0), offset});
    const IR::U32 handle{ir.CompositeExtract(IR::Value{&*load}, 0)};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::ColorCube);
    const auto coordinates = ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f), ir.Imm32(0.0f));
    if (query) {
        const auto dimensions = ir.ImageQueryDimension(handle, constant(0), ir.Imm1(true));
        ir.SetReg(IR::Reg::R2, IR::U32{ir.CompositeExtract(dimensions, 0)});
    } else {
        (void)ir.ImageSampleExplicitLod(handle, coordinates, ir.Imm32(0.0f), {}, flags);
    }
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    const auto tracked = Shader::Optimization::BufferTexture::Track(handle, program.info);
    REQUIRE(tracked);
    REQUIRE(tracked->index.Resolve() == index.Resolve());
    REQUIRE(tracked->stride == 160);
    REQUIRE(tracked->offset == 152);
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    if (failure == 1) env.size -= 1;
    if (failure == 2) env.high = 1;
    if (failure == 3) host.max_per_stage_descriptor_sampled_images = 255;
    if (failure == 4) host.support_buffer_texture_handles = false;
    if (failure) {
        REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
    } else {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        REQUIRE(program.info.texture_descriptors.size() == 1);
        const auto& desc = program.info.texture_descriptors.front();
        REQUIRE(desc.count == 256);
        REQUIRE(desc.cbuf_index == 0);
        REQUIRE(desc.cbuf_offset == 0x510);
        REQUIRE(desc.buffer_source.enabled);
        REQUIRE(desc.buffer_source.offset == 152);
        REQUIRE(desc.buffer_source.stride == 160);
        REQUIRE(desc.buffer_source.size == 40960);
        REQUIRE(desc.buffer_source.width_query == query);
        for (const auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod || inst.GetOpcode() == IR::Opcode::ImageQueryDimensions)
                REQUIRE(inst.Arg(0).Resolve() == index.Resolve());
        }
    }
}

TEST_CASE("Buffer texture choices preserve their select and Phi indices", "[native-shader]") {
    const bool phi_choice = GENERATE(false, true);
    const u32 invalid = GENERATE(0U, 1U, 2U);
    class RootEnvironment final : public EmptyEnvironment {
    public:
        u32 ReadCbufValue(u32 bank, u32 offset) override {
            REQUIRE(bank == 0);
            if (offset == 0x518) return 40960;
            REQUIRE(offset == 0x51c);
            return 0;
        }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block left{pool}, right{pool}, merge{pool};
    IR::Value first_index, second_index;
    const auto make_handle = [&](IR::Block& block, IR::Reg reg, u32 field, IR::Value& original_index) {
        IR::IREmitter ir{block};
        const auto pointer = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
        const auto index = ir.GetReg(reg);
        original_index = index;
        const auto relative = ir.IAdd(ir.IMul(index, ir.Imm32(160U)), ir.Imm32(field));
        const auto offset = ir.ISub(ir.IAdd(pointer, relative), ir.BitwiseAnd(pointer, ir.Imm32(0xfffffff0U)));
        const auto load = block.PrependNewInst(block.end(), IR::Opcode::LoadStorage64, {ir.Imm32(0U), offset});
        return IR::U32{ir.CompositeExtract(IR::Value{&*load}, 0)};
    };
    const auto first = make_handle(left, IR::Reg::R1, 152, first_index);
    const auto second = make_handle(phi_choice ? right : left, IR::Reg::R2, invalid == 2 ? 144 : 152, second_index);
    IR::IREmitter ir{merge};
    IR::IREmitter other_ir{phi_choice ? right : merge};
    const IR::U32 other = invalid == 1 ? other_ir.GetReg(IR::Reg::R8) : second;
    IR::U32 handle;
    if (phi_choice) {
        const auto phi = merge.PrependNewInst(merge.begin(), IR::Opcode::Phi);
        phi->SetFlags(IR::Type::U32);
        phi->AddPhiOperand(&left, first);
        phi->AddPhiOperand(&right, other);
        handle = IR::U32{IR::Value{&*phi}};
    } else handle = IR::U32{ir.Select(ir.GetPred(IR::Pred::P0), first, other)};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::ColorCube);
    (void)ir.ImageSampleExplicitLod(handle,
        ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f), ir.Imm32(0.0f)), ir.Imm32(0.0f), {}, flags);
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&left, &right, &merge};
    program.post_order_blocks = {&merge, &right, &left};
    program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.support_buffer_texture_handles = true;
    if (invalid) REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host), Shader::NotImplementedException);
    else {
        REQUIRE_NOTHROW(Shader::Optimization::TexturePass(env, program, host));
        u32 samples{};
        for (const auto& inst : merge.Instructions()) if (inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod) {
            ++samples;
            const auto* choice = inst.Arg(0).InstRecursive();
            REQUIRE(choice->GetOpcode() == (phi_choice ? IR::Opcode::Phi : IR::Opcode::SelectU32));
            if (phi_choice) {
                REQUIRE(choice->PhiBlock(0) == &left);
                REQUIRE(choice->PhiBlock(1) == &right);
                REQUIRE(choice->Arg(0).Resolve() == first_index.Resolve());
                REQUIRE(choice->Arg(1).Resolve() == second_index.Resolve());
            } else {
                REQUIRE(choice->Arg(0).InstRecursive()->GetOpcode() == IR::Opcode::GetPred);
                REQUIRE(choice->Arg(1).Resolve() == first_index.Resolve());
                REQUIRE(choice->Arg(2).Resolve() == second_index.Resolve());
            }
        }
        REQUIRE(samples == 1);
    }
}

TEST_CASE("Native uniform comparisons and selects preserve independent predicate banks", "[native-shader]") {
    const std::array<Native, 7> input{{
        {{0x09057891, 0x40, 0x0f8e303f, 0}},
        {{0x0500728c, 7, 0x0bf21070, 0}},
        {{0x05057287, 7, 0x08800000, 0}},
        {{0x05057290, 7, 0x0fffe13f, 0}},
        {{0x0400728c, 6, 0x0bf01070, 0}},
        {{0x04047287, 6, 0x08000000, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 width : {100U, 256U}) {
        ScalarNativeMachine machine;
        machine.regs[225] = 2;
        machine.regs[223] = width;
        machine.regs[220] = 40;
        machine.regs[222] = 32;
        for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (p & 1) != 0;
        const auto original = machine.predicates;
        machine.Execute(block);
        REQUIRE(machine.regs[221] == width - std::min(192U, width));
        REQUIRE(machine.regs[220] == 32);
        REQUIRE(((machine.regs[239] & 0x20) != 0) == (192U < width));
        REQUIRE((machine.regs[239] & 0x40) == 0);
        REQUIRE(machine.predicates == original);
    }
}

TEST_CASE("Native global stores carry a register offset into the high address word", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x02007986, 9, 0x0810a906, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[222] = 0xfffffff0;
    machine.regs[223] = 0x21;
    machine.regs[2] = 0x30;
    machine.regs[9] = 0xdeadbeef;
    machine.Execute(block);
    REQUIRE(machine.global == std::unordered_map<u64, u32>{{0x2200000020ULL, 0xdeadbeef}});
}

TEST_CASE("Native high LEA adds the base after unsigned scaling", "[native-shader]") {
    std::array<Native, 2> input{{
        {{0x00007211, 1, 0x078ff8ff, 0}},
        VertexTransform.back(),
    }};
    const u32 shift = GENERATE(0U, 1U, 16U, 31U);
    input[0].words[2] = (input[0].words[2] & ~(31U << 11)) | (shift << 11);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
        for (u32 base : {0U, 1U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[0] = value;
            machine.regs[1] = base;
            machine.Execute(block);
            REQUIRE(machine.regs[0] == static_cast<u32>((u64{value} << shift) >> 32) + base);
        }
    }
}

TEST_CASE("Native LEA predicate carry reaches the high pointer word", "[native-shader]") {
    const std::array<Native, 4> input{{
        {{0x00007a11, 0x800, 0x078010ff, 0}},
        {{0xff017a10, 0x900, 0x007fe4ff, 0}},
        {{0x00007386, 7, 0x0010a900, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 base : {0x100U, 0xfffffff0U}) {
        ScalarNativeMachine machine;
        machine.regs[0] = 8;
        machine.regs[7] = 0x12345678;
        machine.constants[0x20] = base;
        machine.constants[0x24] = 0x21;
        machine.Execute(block);
        REQUIRE(machine.global == std::unordered_map<u64, u32>{{(u64{0x21} << 32) + base + 32, 0x12345678}});
    }
}

TEST_CASE("Native shared atomics and barriers retain real shared-memory effects", "[native-shader]") {
    const std::array<Native, 6> input{{
        {{0x03007388, 0x407, 0x4800, 0}},
        {{0x03097984, 0x400, 0x4800, 0}},
        {{0xff0a738c, 0x7fc09, 0, 0}},
        {{0x00007992, 0, 0x5000, 0}},
        {{0x00007b1d, 0, 0, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[3] = 3;
    machine.regs[7] = 5;
    machine.shared[0x7fc] = 100;
    machine.Execute(block);
    REQUIRE(machine.shared.at(16) == 5);
    REQUIRE(machine.shared.at(0x7fc) == 105);
    REQUIRE(machine.regs[10] == 100);
    REQUIRE(machine.workgroup_barriers == 1);
    REQUIRE(machine.device_barriers == 1);
}

TEST_CASE("Native uniform high shifts and multiply operands retain numeric values", "[native-shader]") {
    std::array<Native, 7> input{{
        {{0x00057882, 123, 0, 0}},
        {{0x3f047899, 5, 0x08010605, 0}},
        {{0x05057899, 5, 0x0800063f, 0}},
        {{0x01027c24, 5, 0x0f8e0202, 0}},
        {{0x00007c17, 4, 0x0b800200, 0}},
        {{0x000a7424, 4, 0x078e0205, 0}},
        VertexTransform.back(),
    }};
    for (u32 value : {0U, 1U, 123U, 0x07ffffffU, 0xffffffffU}) {
        input[0].words[1] = value;
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        ScalarNativeMachine machine;
        machine.regs[0] = 5000;
        machine.regs[1] = 11;
        machine.regs[2] = 7;
        machine.regs[5] = 3;
        machine.Execute(block);
        const u32 shifted = value << 5;
        const u32 minimum = static_cast<u32>(std::min(5000, static_cast<s32>(shifted)));
        REQUIRE(machine.regs[220] == shifted);
        REQUIRE(machine.regs[221] == shifted);
        REQUIRE(machine.regs[2] == 11U * shifted + 7);
        REQUIRE(machine.regs[0] == minimum);
        REQUIRE(machine.regs[10] == minimum * 3U + 4);
    }
}

TEST_CASE("Native uniform immediate multiply preserves wraparound and aliased operands", "[native-shader]") {
    const u32 multiplier = GENERATE(0U, 1U, 5U, 48U, 0xffffffffU);
    const std::array<Native, 2> input{{
        {{0x040478a4, multiplier, 0x0f8e023f, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 123U, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[220] = value;
        machine.regs[221] = 0xcafebeef;
        machine.predicates.fill(true);
        machine.Execute(block);
        REQUIRE(machine.regs[220] == value * multiplier);
        REQUIRE(machine.regs[221] == 0xcafebeef);
        REQUIRE(std::ranges::all_of(machine.predicates, [](bool p) { return p; }));
    }
}

TEST_CASE("Native integer absolute values preserve the minimum integer and source aliases", "[native-shader]") {
    const u32 destination = GENERATE(5U, 17U);
    const std::array<Native, 2> input{{
        {{0x7213U | (destination << 16), 17, 0, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bits : {0U, 1U, 0xffffffffU, 0x80000000U, 0x7fffffffU, 0xfedcba98U}) {
        ScalarNativeMachine machine;
        machine.regs[17] = bits;
        machine.regs[18] = 0x12345678;
        machine.Execute(block);
        const u32 expected = bits & 0x80000000U ? 0U - bits : bits;
        REQUIRE(machine.regs[destination] == expected);
        REQUIRE(machine.regs[17] == (destination == 17 ? expected : bits));
        REQUIRE(machine.regs[18] == 0x12345678);
    }
}

TEST_CASE("Native predicate AND with an inverted aliased input preserves other predicates", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0x781c, 0, 0x00f20170, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (bool p0 : {false, true}) for (bool p1 : {false, true}) {
        ScalarNativeMachine machine;
        for (u32 i = 0; i < 7; ++i) machine.predicates[i] = (i & 1) != 0;
        machine.predicates[0] = p0;
        machine.predicates[1] = p1;
        auto expected = machine.predicates;
        expected[1] = p0 && !p1;
        machine.Execute(block);
        REQUIRE(machine.predicates == expected);
    }
}

TEST_CASE("Native bit reversal and register shifts preserve FFT indexing", "[native-shader]") {
    const std::array<Native, 6> input{{
        {{0x00037301, 19, 0, 0}},
        {{0x03017211, 255, 0x078f50ff, 0}},
        {{0xff087219, 4, 0x00011613, 0}},
        {{0x0f0f7219, 12, 0x00000eff, 0}},
        {{0xff047224, 255, 0x078e0a0c, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 shift : {0U, 1U, 10U, 31U, 32U, 33U, 63U}) {
        ScalarNativeMachine machine;
        machine.regs[19] = 0x12345678;
        machine.regs[4] = shift;
        machine.regs[12] = shift;
        machine.regs[15] = 0x9abcdef0;
        machine.Execute(block);
        REQUIRE(machine.regs[3] == 0x1e6a2c48);
        REQUIRE(machine.regs[1] == (0x1e6a2c48U >> 22));
        REQUIRE(machine.regs[8] == (shift < 32 ? 0x12345678U >> shift : 0));
        REQUIRE(machine.regs[15] == (0x9abcdef0U << (shift & 31)));
        REQUIRE(machine.regs[4] == 0U - shift);
    }
}

TEST_CASE("Native constant shift counts wrap at the word width", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0xff097a19, 0x01003500, 0x00011e0a, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 shift : {0U, 1U, 10U, 31U, 32U, 33U, 63U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[10] = 0xfedcba98;
        machine.constants[(u64{4} << 32) | 0xd4] = shift;
        machine.Execute(block);
        REQUIRE(machine.regs[9] == (0xfedcba98U >> (shift & 31)));
        REQUIRE(machine.regs[10] == 0xfedcba98);
    }
}

TEST_CASE("Native global vector loads preserve both address words and each result channel", "[native-shader]") {
    const bool pair = GENERATE(false, true);
    const u32 offset = GENERATE(0U, 16U, 0xfffff0U);
    const u32 destination = GENERATE(8U, 16U);
    const std::array<Native, 2> input{{
        {{0x08007381U | (destination << 16), offset << 8, pair ? 0x001e8b00U : 0x001e8d00U, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    std::vector<u32> reads, writes, channels;
    u32 loads{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister) reads.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == (pair ? IR::Opcode::LoadGlobal64 : IR::Opcode::LoadGlobal128)) {
            ++loads;
            const auto* address = inst.Arg(0).Inst();
            REQUIRE(address->GetOpcode() == IR::Opcode::IAdd64);
            REQUIRE(address->Arg(1).U64() == static_cast<u64>(static_cast<s64>(static_cast<s32>(offset << 8) >> 8)));
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
            const auto* extract = inst.Arg(1).Inst();
            REQUIRE(extract->GetOpcode() == (pair ? IR::Opcode::CompositeExtractU32x2 : IR::Opcode::CompositeExtractU32x4));
            channels.push_back(extract->Arg(1).U32());
        }
    }
    REQUIRE(loads == 1);
    std::ranges::sort(reads);
    REQUIRE(reads == std::vector<u32>{8, 9});
    REQUIRE(writes == (pair ? std::vector<u32>{destination, destination + 1} : std::vector<u32>{destination, destination + 1, destination + 2, destination + 3}));
    REQUIRE(channels == (pair ? std::vector<u32>{0, 1} : std::vector<u32>{0, 1, 2, 3}));
    auto invalid = input;
    invalid[0].words[0] |= 1U << 24;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native shared atomic OR retains the shared offset and source bits", "[native-shader]") {
    const std::array<Native, 2> input{{
        {{0xffff738c, 0x6807, 0x03000000, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 initial : {0U, 0x12345678U, 0xffffffffU}) {
        for (u32 operand : {0U, 0x87654321U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[7] = operand;
            machine.shared[0x68] = initial;
            machine.Execute(block);
            REQUIRE(machine.shared.at(0x68) == (initial | operand));
            REQUIRE(machine.regs[7] == operand);
        }
    }
}

TEST_CASE("Native immediate high-word shifts preserve wrapped and clamped bit masks", "[native-shader]") {
    const bool wrap = GENERATE(false, true);
    const u32 value = GENERATE(1U, 0x87654321U, 0xffffffffU);
    const std::array<Native, 2> input{{
        {{0xff057419, value, 0x00010603U | (u32{wrap} << 11), 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 shift : {0U, 1U, 10U, 31U, 32U, 33U, 63U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[3] = shift;
        machine.Execute(block);
        REQUIRE(machine.regs[5] == (wrap ? (value << (shift & 31)) :
                                             (shift < 32 ? value << shift : 0U)));
        REQUIRE(machine.regs[3] == shift);
    }
}

TEST_CASE("Native scalar global loads retain address carry and signed offsets", "[native-shader]") {
    const u32 offset = GENERATE(0U, 16U, 0xfffff0U);
    const std::array<Native, 2> input{{
        {{0x001d7381, offset << 8, 0x001e8900, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[0] = 0xfffffff8;
    machine.regs[1] = 0x21;
    const u64 displacement = static_cast<u64>(static_cast<s64>(static_cast<s32>(offset << 8) >> 8));
    machine.global[0x21fffffff8ULL + displacement] = 0x87654321;
    machine.Execute(block);
    REQUIRE(machine.regs[29] == 0x87654321);
    REQUIRE(machine.regs[0] == 0xfffffff8);
    REQUIRE(machine.regs[1] == 0x21);
}

TEST_CASE("Native global loads combine a uniform pointer with the scalar byte displacement", "[native-shader]") {
    const bool wide_offset = GENERATE(false, true);
    const u32 offset = GENERATE(0U, 16U, 0xfffff0U);
    const u32 source = wide_offset ? 32U : 33U;
    const std::array<Native, 2> input{{
        {{0x001d7981U | (source << 24), 4U | (offset << 8),
          0x081e8900U | (u32{wide_offset} << 26), 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[220] = 0xfffffff8;
    machine.regs[221] = 0x21;
    machine.regs[32] = 16;
    machine.regs[33] = wide_offset ? 1 : 16;
    const u64 displacement = static_cast<u64>(static_cast<s64>(static_cast<s32>(offset << 8) >> 8));
    machine.global[0x21fffffff8ULL + (wide_offset ? 0x100000010ULL : 16ULL) + displacement] = 0x87654321;
    machine.Execute(block);
    REQUIRE(machine.regs[29] == 0x87654321);
    REQUIRE(machine.regs[220] == 0xfffffff8);
    REQUIRE(machine.regs[221] == 0x21);
    REQUIRE(machine.regs[source] == 16);
}

TEST_CASE("Native paired shared values keep both words under scaled addressing", "[native-shader]") {
    const std::array<Native, 3> input{{
        {{0x13007388, 4, 0x8a00, 0}},
        {{0x13027984, 0, 0x8a00, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[19] = 17;
    machine.regs[4] = 0x12345678;
    machine.regs[5] = 0x9abcdef0;
    machine.Execute(block);
    REQUIRE(machine.shared.at(17 * 8) == 0x12345678);
    REQUIRE(machine.shared.at(17 * 8 + 4) == 0x9abcdef0);
    REQUIRE(machine.regs[2] == 0x12345678);
    REQUIRE(machine.regs[3] == 0x9abcdef0);
}

TEST_CASE("Native formatted surface stores retain coordinates channels and descriptor", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00007b9b, 0x60000a04, 0x00100f00, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 writes{};
    for (const auto& inst : block.Instructions()) if (inst.GetOpcode() == IR::Opcode::BoundImageWrite) {
        REQUIRE(inst.Arg(0).U32() == 40);
        const auto info = inst.Flags<IR::TextureInstInfo>();
        REQUIRE(info.type.Value() == Shader::TextureType::Color2D);
        const auto* coordinates = inst.Arg(1).Inst();
        REQUIRE(coordinates->Arg(0).Inst()->Arg(0).Reg() == IR::Reg::R0);
        REQUIRE(coordinates->Arg(1).Inst()->Arg(0).Reg() == IR::Reg::R1);
        const auto* color = inst.Arg(2).Inst();
        for (u32 component = 0; component < 4; ++component)
            REQUIRE(static_cast<u32>(color->Arg(component).Inst()->Arg(0).Reg()) == 4 + component);
        ++writes;
    }
    REQUIRE(writes == 1);
}

TEST_CASE("Native predicated predicate copies retain their guard and source", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0000281c, 0, 0x01f0f070, 0}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    const auto predicate = Shader::Maxwell::Instruction{lowered.instructions.front()}.Pred();
    REQUIRE(predicate.index == 2);
    REQUIRE_FALSE(predicate.negated);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (bool source : {false, true}) {
        ScalarNativeMachine machine;
        machine.predicates[3] = source;
        machine.Execute(block);
        REQUIRE(machine.predicates[0] == source);
    }
}

TEST_CASE("Native NTZ nearest conversion rounds ties to even and clears NaNs", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00117305, 2, 0x00203100, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 rounds{};
    for (const auto& inst : block.Instructions()) if (inst.GetOpcode() == IR::Opcode::FPRoundEven32) {
        REQUIRE(inst.Flags<IR::FpControl>().fmz_mode == IR::FmzMode::None);
        ++rounds;
    }
    REQUIRE(rounds == 1);
    for (const auto [value, expected] : std::array<std::pair<float, s32>, 9>{{
        {0.5f, 0}, {1.5f, 2}, {2.5f, 2}, {-0.5f, 0}, {-1.5f, -2}, {-2.5f, -2},
        {0.9f, 1}, {-0.9f, -1}, {std::numeric_limits<float>::quiet_NaN(), 0},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[2] = std::bit_cast<u32>(value);
        machine.Execute(block);
        REQUIRE(machine.regs[17] == static_cast<u32>(expected));
    }
    auto unsupported = input;
    unsupported[0].words[2] ^= 1U << 13;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
}

TEST_CASE("Native predicate packing merges only selected predicate bits", "[native-shader]") {
    for (u32 mask : {0U, 8U, 16U, 0x55U, 0x7fU}) {
        const std::array<Native, 2> input{{{{0x02137803, mask, 0, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 bits = 0; bits < 128; ++bits) {
            ScalarNativeMachine machine;
            machine.regs[2] = 0xa5a5a5a5U;
            for (u32 i = 0; i < 7; ++i) machine.predicates[i] = (bits >> i) & 1;
            const auto saved = machine.predicates;
            machine.Execute(block);
            REQUIRE(machine.regs[19] == ((0xa5a5a5a5U & ~mask) | (bits & mask)));
            REQUIRE(machine.predicates == saved);
        }
    }
}

TEST_CASE("Native signed NTZ floor conversion rounds negative fractions down", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00237305, 0x24, 0x00207100, 0x000e2400}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (const auto [value, expected] : std::array<std::pair<float, s32>, 8>{{
        {1.9f, 1}, {-1.1f, -2}, {-0.1f, -1}, {0.9f, 0},
        {0.0f, 0}, {-2.0f, -2}, {12345.5f, 12345},
        {std::numeric_limits<float>::quiet_NaN(), 0},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[36] = std::bit_cast<u32>(value);
        machine.Execute(block);
        REQUIRE(machine.regs[35] == static_cast<u32>(expected));
    }
}

TEST_CASE("Native dependency waits retain the full six-bit pending-operation count", "[native-shader]") {
    for (u32 count : {0U, 1U, 15U, 22U, 63U}) {
        const std::array<Native, 2> input{{{{0x0000791a, 0xd000U | (count << 6), 0, 0}}, VertexTransform.back()}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 1);
    }
    const std::array<Native, 2> unsupported{{{{0x0000791a, 0xc000, 0, 0}}, VertexTransform.back()}};
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
}

TEST_CASE("Native histogram warp match compares both address words and preserves predicates", "[native-shader]") {
    const bool wide = GENERATE(false, true);
    const u32 target = GENERATE(0U, 1U, 5U);
    const u32 source = wide ? 0U : 3U;
    const std::array<Native, 2> input{{{{0x00ff73a1U | (source << 24), 0,
        (target << 17) | (wide ? 0x200U : 0U), 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 active : {1U, 0x80000000U, 0x80000001U, 0x55555555U, 0xffffffffU}) {
        for (u32 mismatch : {0U, 1U, 2U}) {
            std::array<std::array<u32, 256>, 32> registers{};
            std::array<std::array<bool, 7>, 32> predicates{};
            const u32 first = std::countr_zero(active);
            for (u32 lane = 0; lane < 32; ++lane) {
                registers[lane][source] = 0xfffffff0U + (mismatch == 1 && lane != first ? 4 : 0);
                registers[lane][source + 1] = 0x21U + (mismatch == 2 && lane != first ? 1 : 0);
                for (u32 p = 0; p < 7; ++p) predicates[lane][p] = ((lane + p) & 1) != 0;
            }
            const auto original = predicates;
            ExecuteNativeWarp(block, active, registers, predicates);
            for (u32 lane = 0; lane < 32; ++lane) {
                auto expected = original[lane];
                if (active & (1U << lane)) expected[target] = mismatch == 0 || (!wide && mismatch == 2) || std::popcount(active) == 1;
                REQUIRE(predicates[lane] == expected);
            }
        }
    }
}

TEST_CASE("Native warp sum OR and unsigned extrema reduce only active lanes", "[native-shader]") {
    const u32 operation = GENERATE(0U, 1U, 2U, 3U);
    std::array<Native, 2> input{{{{0x030573c4, 0, 0x0000c000, 0}}, VertexTransform.back()}};
    if (operation == 1) input[0].words[2] = 0x4000;
    if (operation == 2) input[0].words[2] = 0x10000;
    if (operation == 3) input[0].words[2] = 0x14000;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 active : {1U, 0x80000000U, 0x80000001U, 0x55555555U, 0xffffffffU}) {
        std::array<std::array<u32, 256>, 32> registers{};
        std::array<std::array<bool, 7>, 32> predicates{};
        u32 sum = operation == 2 ? 0xffffffffU : 0U;
        for (u32 lane = 0; lane < 32; ++lane) {
            registers[lane][3] = 0xfffffff0U + lane * 17;
            registers[lane][221] = 0xcafebeef;
            if (active & (1U << lane)) sum = operation == 3 ? std::max(sum, registers[lane][3]) : operation == 2 ? std::min(sum, registers[lane][3]) : operation == 1 ? sum | registers[lane][3] : sum + registers[lane][3];
            for (u32 p = 0; p < 7; ++p) predicates[lane][p] = ((lane + p) & 1) != 0;
        }
        const auto original = predicates;
        ExecuteNativeWarp(block, active, registers, predicates);
        REQUIRE(predicates == original);
        for (u32 lane = 0; lane < 32; ++lane)
            REQUIRE(registers[lane][221] == ((active & (1U << lane)) ? sum : 0xcafebeef));
    }
}

TEST_CASE("Native histogram pointer carry and global reduction retain the address and addend", "[native-shader]") {
    const std::array<Native, 4> input{{
        {{0x00007a10, 0x00001000, 0x07f1e0ff, 0}},
        {{0xff017624, 0x00001100, 0x000e06ff, 0}},
        {{0x0000798e, 3, 0x0010e100, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 low : {0U, 0xfffffff0U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[0] = 32;
        machine.regs[3] = 17;
        machine.constants[0x40] = low;
        machine.constants[0x44] = 0x21;
        const u64 address = 0x2100000000ULL + low + 32;
        machine.global[address] = 0xfffffff8U;
        machine.Execute(block);
        REQUIRE(machine.regs[0] == static_cast<u32>(address));
        REQUIRE(machine.regs[1] == address >> 32);
        REQUIRE(machine.predicates[0] == (low > 0xffffffdfU));
        REQUIRE(machine.global == std::unordered_map<u64, u32>{{address, 9}});
    }
}

TEST_CASE("Native histogram shared increment scales its address and increments once", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x03ff7f8c, 0, 0x0d00403f, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[3] = 63;
    machine.shared[252] = 0xffffffffU;
    machine.Execute(block);
    REQUIRE(machine.shared == std::unordered_map<u64, u32>{{252, 0}});
}

TEST_CASE("Native histogram uniform leading bit and comparison select the highest active lane", "[native-shader]") {
    const std::array<Native, 3> input{{{{0x000472bd, 4, 0x080e0000, 0}},
        {{0x02007c0c, 4, 0x0bf02070, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 mask : {1U, 0x80000000U, 0x55555555U, 0xffffffffU}) {
        for (u32 lane = 0; lane < 32; ++lane) {
            ScalarNativeMachine machine;
            machine.regs[220] = mask;
            machine.regs[2] = lane;
            machine.Execute(block);
            REQUIRE(machine.regs[220] == 31U - std::countl_zero(mask));
            REQUIRE(machine.predicates[0] == (lane == 31U - std::countl_zero(mask)));
        }
    }
}

TEST_CASE("Native formatted surface loads retain selected channels coordinates descriptor and guard", "[native-shader]") {
    const Native instruction = GENERATE(Native{{0x400a0b97, 0x60001200, 0x001e0100, 0}},
                                        Native{{0x12250b97, 0x60001200, 0x001e0100, 0}},
                                        Native{{0x06087b97, 0x60002000, 0x001e0f00, 0}});
    const std::array<Native, 2> input{{instruction, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 0);
    const auto guard = Shader::Maxwell::Instruction{lowered.instructions.front()}.Pred();
    REQUIRE(guard.index == instruction.Bits(12, 3));
    REQUIRE_FALSE(guard.negated);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 loads{}, writes{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageRead) {
            REQUIRE(inst.Arg(0).U32() == instruction.Bits(40, 14) * 4);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color2D);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(static_cast<u32>(coords->Arg(0).Inst()->Arg(0).Reg()) == instruction.Bits(24, 8));
            REQUIRE(static_cast<u32>(coords->Arg(1).Inst()->Arg(0).Reg()) == instruction.Bits(24, 8) + 1);
            ++loads;
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            REQUIRE(static_cast<u32>(inst.Arg(0).Reg()) == instruction.Bits(16, 8) + writes);
            const auto* red = inst.Arg(1).Inst();
            REQUIRE(red->GetOpcode() == IR::Opcode::CompositeExtractU32x4);
            REQUIRE(red->Arg(1).U32() == writes);
            REQUIRE(red->Arg(0).Inst()->GetOpcode() == IR::Opcode::BoundImageRead);
            ++writes;
        }
    }
    REQUIRE(loads == 1);
    REQUIRE(writes == (instruction.Bits(72, 4) == 15 ? 4U : 1U));
    auto invalid = input;
    invalid[0].words[2] ^= 0x200; // Reject other channel masks until implemented.
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native volume texel fetch preserves XYZ LOD descriptor and split RGBA outputs", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00087b66, 0x50000a02, 0x049e0f0a, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 fetches{};
    std::vector<u32> writes;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageFetch) {
            REQUIRE(inst.Arg(0).U32() == 40);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color3D);
            const auto* coords = inst.Arg(1).Inst();
            for (u32 i = 0; i < 3; ++i)
                REQUIRE(static_cast<u32>(coords->Arg(i).Inst()->Arg(0).Reg()) == i);
            REQUIRE(inst.Arg(3).U32() == 0);
            ++fetches;
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
            const auto* extract = inst.Arg(1).Inst()->Arg(0).Inst();
            REQUIRE(extract->GetOpcode() == IR::Opcode::CompositeExtractF32x4);
            REQUIRE(extract->Arg(1).U32() == writes.size() - 1);
        }
    }
    REQUIRE(fetches == 1);
    REQUIRE(writes == std::vector<u32>{8, 9, 10, 11});
    auto invalid = input;
    invalid[0].words[0] |= 1U << 24;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Native volume surface store retains XYZ and all four data channels", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00007b9b, 0xa0000c04, 0x00100f00, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 stores{};
    for (const auto& inst : block.Instructions()) if (inst.GetOpcode() == IR::Opcode::BoundImageWrite) {
        REQUIRE(inst.Arg(0).U32() == 48);
        REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color3D);
        for (u32 i = 0; i < 3; ++i)
            REQUIRE(static_cast<u32>(inst.Arg(1).Inst()->Arg(i).Inst()->Arg(0).Reg()) == i);
        for (u32 i = 0; i < 4; ++i)
            REQUIRE(static_cast<u32>(inst.Arg(2).Inst()->Arg(i).Inst()->Arg(0).Reg()) == 4 + i);
        ++stores;
    }
    REQUIRE(stores == 1);
}

TEST_CASE("Native half FMA ReLU preserves fusion values above one and canonical NaNs", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0d127231, 0x3000000d, 0x03848d10, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    const std::array<std::array<u16, 3>, 9> cases{{
        {{0x3c00, 0x4400, 0x4200}}, // -1*1+4 = 3, not saturated to one
        {{0x4000, 0x3c00, 0x0000}}, // -2*2+1 < 0
        {{0x3c00, 0x3c00, 0x0000}},
        {{0x0000, 0x0001, 0x0001}}, // smallest positive subnormal survives
        {{0x7c00, 0x0000, 0x0000}},
        {{0x0000, 0x7c00, 0x7c00}},
        {{0x7e01, 0x3c00, 0x7fff}},
        {{0xfe01, 0x3c00, 0x7fff}},
        {{0x8000, 0x8000, 0x0000}},
    }};
    for (const auto& values : cases) {
        ScalarNativeMachine machine;
        machine.regs[13] = (u32{values[0]} << 16) | 0x4400;
        machine.regs[16] = 0xc0000000 | values[1];
        machine.predicates = {true, false, true, false, true, false, true};
        const auto predicates = machine.predicates;
        machine.Execute(block);
        REQUIRE(machine.regs[18] == (u32{values[2]} | (u32{values[2]} << 16)));
        REQUIRE(machine.predicates == predicates);
    }
    // A separate register operand permits a fused product residual that would
    // vanish if the multiplication rounded before adding its third operand.
    auto fused = input;
    fused[0].words[1] = 0x3000000e;
    IR::Block fused_block{pool};
    TranslateScalarNative(fused, fused_block);
    ScalarNativeMachine machine;
    machine.regs[13] = 0x3c013c01;
    machine.regs[14] = 0x3bfe3bfe;
    machine.regs[16] = 0x3c003c00;
    machine.Execute(fused_block);
    REQUIRE(machine.regs[18] == 0x00100010); // 2^-20 in each half
}

TEST_CASE("Native half FMA ReLU accepts packed immediates without losing input aliases", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x02027831, 0x3c003c00, 0x03968805, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (const auto [source, addend, expected] : std::array<std::array<u16, 3>, 4>{{
        {{0x4000, 0x3c00, 0x3c00}}, {{0x3c00, 0x4400, 0x0000}},
        {{0x4400, 0x3c00, 0x4200}}, {{0x7e01, 0x3c00, 0x7fff}},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[2] = source | 0xc4000000U;
        machine.regs[5] = (u32{addend} << 16) | 0x4800;
        machine.Execute(block);
        REQUIRE(machine.regs[2] == (u32{expected} | (u32{expected} << 16)));
    }
}

TEST_CASE("Native quad butterfly and swizzled add retain horizontal and vertical differences", "[native-shader]") {
    const bool vertical = GENERATE(false, true);
    const std::array<Native, 3> input{{
        {{0x05107f89, vertical ? 0x0c5c0300U : 0x0c3c0300U, 0x000e0000, 0}},
        {{0x100c7822, vertical ? 0x5aU : 0x66U, 5, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    std::array<std::array<u32, 256>, 32> registers{};
    std::array<std::array<bool, 7>, 32> predicates{};
    for (u32 lane = 0; lane < 32; ++lane)
        registers[lane][5] = std::bit_cast<u32>(static_cast<float>(lane / 4 * 100 + (lane & 1) * 3 + ((lane >> 1) & 1) * 11));
    ExecuteNativeWarp(block, 0xffffffffU, registers, predicates);
    for (u32 lane = 0; lane < 32; ++lane) {
        REQUIRE(registers[lane][16] == registers[lane ^ (vertical ? 2 : 1)][5]);
        REQUIRE(std::bit_cast<float>(registers[lane][12]) == (vertical ? 11.0f : 3.0f));
    }
}

TEST_CASE("Native array gradient fetch retains array index XY derivatives descriptor and RGBA", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x04047b6c, 0xa080080c, 0x001e0f06, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 samples{};
    std::vector<u32> writes;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageGradient) {
            REQUIRE(inst.Arg(0).U32() == 32);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorArray2D);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().num_derivatives.Value() == 2);
            const auto* coords = inst.Arg(1).Inst();
            REQUIRE(coords->Arg(0).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{4});
            REQUIRE(coords->Arg(1).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{5});
            const auto* layer = coords->Arg(2).Inst()->Arg(0).Inst();
            REQUIRE(layer->GetOpcode() == IR::Opcode::BitFieldUExtract);
            REQUIRE(layer->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{6});
            REQUIRE(layer->Arg(2).U32() == 16);
            for (u32 component = 0; component < 4; ++component)
                REQUIRE(inst.Arg(2).Inst()->Arg(component).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{12 + component});
            ++samples;
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
    }
    REQUIRE(samples == 1);
    REQUIRE(writes == std::vector<u32>{4, 5, 6, 7});
}

TEST_CASE("Native capture distinguishes forward branches from call targets beyond main EXIT", "[native-shader]") {
    using Capture = Shader::Ampere::ProgramCaptureState;
    Capture calls;
    REQUIRE(calls.Observe(Native{{0x7944, 48, 0x03c00000, 0}}, 1) == Capture::Result::Continue);
    REQUIRE(calls.Observe(VertexTransform.back(), 2) == Capture::Result::Continue);
    REQUIRE(calls.Observe(Native{{0x08007950, 0, 0, 0}}, 6) == Capture::Result::Complete);
    Capture branches;
    REQUIRE(branches.Observe(Native{{0x7947, 48, 0x03800000, 0}}, 0) == Capture::Result::Continue);
    REQUIRE(branches.Observe(VertexTransform.back(), 1) == Capture::Result::Continue);
    REQUIRE(branches.Observe(VertexTransform.back(), 4) == Capture::Result::Complete);
    Capture multiple;
    REQUIRE(multiple.Observe(Native{{0x7944, 96, 0x03c00000, 0}}, 1) == Capture::Result::Continue);
    REQUIRE(multiple.Observe(Native{{0x7944, 128, 0x03c00000, 0}}, 3) == Capture::Result::Continue);
    REQUIRE(multiple.Observe(VertexTransform.back(), 4) == Capture::Result::Continue);
    REQUIRE(multiple.Observe(Native{{0x08007950, 0, 0, 0}}, 9) == Capture::Result::Continue);
    REQUIRE(multiple.Observe(Native{{0x08007950, 0, 0, 0}}, 13) == Capture::Result::Complete);
    Capture invalid;
    REQUIRE(invalid.Observe(Native{{0x7947, 1, 0x03800000, 0}}, 0) == Capture::Result::InvalidTarget);
}

TEST_CASE("Native implicit volume sample preserves XYZ and all four split channels", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0a0a7b60, 0x5080400e, 0x041e0f0c, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 samples{};
    std::vector<std::pair<u32, u32>> copies;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageSampleImplicitLod) {
            REQUIRE(inst.Arg(0).U32() == 256);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::Color3D);
            for (u32 i = 0; i < 3; ++i)
                REQUIRE(inst.Arg(1).Inst()->Arg(i).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250 + i});
            ++samples;
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister && inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
    }
    REQUIRE(samples == 1);
    REQUIRE(copies == std::vector<std::pair<u32, u32>>{{250, 10}, {251, 11}, {252, 14}, {10, 250}, {11, 251}, {12, 252}, {13, 253}});
}

TEST_CASE("Native array shadow sampling retains layer XY depth reference and scalar destination", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x082e7b60, 0xb080140a, 0x001e41ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 samples{};
    std::vector<std::pair<u32, u32>> copies;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BoundImageSampleDrefImplicitLod) {
            REQUIRE(inst.Arg(0).U32() == 80);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorArray2D);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().is_depth.Value() == 1);
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{251});
            REQUIRE(inst.Arg(1).Inst()->Arg(1).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{252});
            REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{11});
            ++samples;
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister && inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetRegister)
            copies.emplace_back(static_cast<u32>(inst.Arg(0).Reg()), static_cast<u32>(inst.Arg(1).Inst()->Arg(0).Reg()));
    }
    REQUIRE(samples == 1);
    REQUIRE(copies == std::vector<std::pair<u32, u32>>{{250, 8}, {251, 9}, {252, 10}});
    REQUIRE(block.Instructions().back().Arg(0).Reg() == IR::Reg{46});
}

TEST_CASE("Native immediate uniform comparison and select preserve predicates at unsigned boundaries", "[native-shader]") {
    const std::array<Native, 3> input{{{{0x0400788c, 6, 0x0bf01070, 0}}, {{0x04057887, 6, 0x08000000, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 5U, 6U, 7U, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[220] = value;
        for (u32 i = 0; i < 7; ++i) machine.predicates[i] = (i & 1) != 0;
        const auto saved = machine.predicates;
        machine.Execute(block);
        REQUIRE(((machine.regs[239] >> 6) & 1) == (value < 6));
        REQUIRE(machine.regs[221] == std::min(value, 6U));
        REQUIRE(machine.predicates == saved);
    }
}

TEST_CASE("Native uniform signed right shift sign extends the high word", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x3f057899, 31, 0x08011404, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[220] = value;
        machine.Execute(block);
        REQUIRE(machine.regs[221] == ((value & 0x80000000U) ? 0xffffffffU : 0U));
    }
}

TEST_CASE("Native indexed constant load adds a signed byte displacement", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x30077b82, 0x00fe0500, 0x00000800, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[48] = 0x800;
    machine.constants[(u64{3} << 32) | 0x14] = 0x12345678;
    machine.Execute(block);
    REQUIRE(machine.regs[7] == 0x12345678);
}

TEST_CASE("Native local spill retains byte address data and execution predicate", "[native-shader]") {
    const u32 offset = GENERATE(0U, 4U, 28U);
    std::array<Native, 3> input{{{{0xff001387, 85, 0x00100800, 0}},
        {{0xff2a7983, 0, 0x00300800, 0}}, VertexTransform.back()}};
    input[0].words[1] |= offset << 8;
    input[1].words[1] |= offset << 8;
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(Shader::Maxwell::Decode(lowered.instructions[0]) == Shader::Maxwell::Opcode::STL);
    REQUIRE(Shader::Maxwell::Instruction{lowered.instructions[0]}.Pred().index == 1);
    REQUIRE((lowered.instructions[0] & 255) == 85);
    REQUIRE((lowered.instructions[1] & 255) == 42);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    class LocalEnvironment : public EmptyEnvironment {
    public:
        u32 LocalMemorySize() const override { return 32; }
    } env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.STL(lowered.instructions[0]);
    visitor.LDL(lowered.instructions[1]);
    u32 loads{}, stores{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::LoadLocal) {
            REQUIRE(inst.Arg(0).U32() == offset / 4);
            ++loads;
        }
        if (inst.GetOpcode() == IR::Opcode::WriteLocal) {
            REQUIRE(inst.Arg(0).U32() == offset / 4);
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg{85});
            ++stores;
        }
    }
    REQUIRE(loads == 1);
    REQUIRE(stores == 1);
    input[0].words[1] |= 1U << 8;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
}

TEST_CASE("Native scalar half conversion applies absolute value before rounding", "[native-shader]") {
    const bool negate = GENERATE(false, true);
    const std::array<Native, 2> input{{{{0x00047304, negate ? 0x80000000U : 0x40000000U, 0x00200800, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (const auto [value, expected] : std::array<std::pair<float, u32>, 7>{{
        {-1.0f, 0x3c00}, {-0.0f, 0}, {-2.5f, 0x4100},
        {1.00048828125f, 0x3c00}, {1.00146484375f, 0x3c02},
        {-0x1p-24f, 1}, {-std::numeric_limits<float>::infinity(), 0x7c00},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[0] = std::bit_cast<u32>(value);
        machine.Execute(block);
        REQUIRE(machine.regs[4] == (expected | (negate && !std::signbit(value) ? 0x8000U : 0U)));
    }
}

TEST_CASE("Native predicated half ReLU guards its complete expansion including predicate scratch", "[native-shader]") {
    for (u32 pred : {1U, 6U, 9U, 14U}) {
        const std::array<Native, 2> input{{{{0x12140231U | (pred << 12), 0x20000012, 0x03848914, 0}}, VertexTransform.back()}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        REQUIRE(lowered);
        REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::BRA);
        REQUIRE(((lowered.instructions.front() >> 16) & 15) == (pred ^ 8));
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        EmptyEnvironment env;
        Shader::Maxwell::TranslatorVisitor visitor{env, block};
        for (size_t i = 1; i + 1 < lowered.instructions.size(); ++i) {
            const u64 word = lowered.instructions[i];
            REQUIRE(((word >> 16) & 15) == 7);
            switch (Shader::Maxwell::Decode(word)) {
#define INST(name, cute, encode) case Shader::Maxwell::Opcode::name: visitor.name(word); break;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef INST
            }
        }
        for (bool value : {false, true}) {
            ScalarNativeMachine machine;
            machine.regs[18] = 0x3c003c00;
            machine.regs[20] = 0x44004400;
            machine.predicates.fill(true);
            machine.predicates[pred & 7] = value;
            const auto saved = machine.predicates;
            if (value != ((pred & 8) != 0)) machine.Execute(block);
            REQUIRE(machine.regs[20] == (value != ((pred & 8) != 0) ? 0x42004200U : 0x44004400U));
            REQUIRE(machine.predicates == saved);
        }
    }
}

TEST_CASE("Native unsigned half conversion truncates and saturates NTZ edge cases", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x000e7305, 13, 0x0010f000, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (const auto [value, expected] : std::array<std::pair<u32, u32>, 8>{{
        {0x3e00, 1}, {0x4100, 2}, {0xbe00, 0}, {0x0001, 0},
        {0x7bff, 65504}, {0x7c00, 0xffffffffU}, {0xfc00, 0}, {0x7e00, 0},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[13] = 0xdead0000 | value;
        machine.Execute(block);
        REQUIRE(machine.regs[14] == expected);
    }
}

TEST_CASE("Native predicate LUT OR combines three inputs while preserving other predicates", "[native-shader]") {
    const std::array<Native, 3> input{{{{0x0000781c, 0, 0x01f25f16, 0}}, {{0x0000781c, 0, 0x00703570, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bits = 0; bits < 128; ++bits) {
        ScalarNativeMachine machine;
        for (u32 i = 0; i < 7; ++i) machine.predicates[i] = (bits >> i) & 1;
        auto expected = machine.predicates;
        expected[1] = expected[1] || expected[2] || expected[3];
        expected[0] = expected[0] || expected[1];
        machine.Execute(block);
        REQUIRE(machine.predicates == expected);
    }
}

TEST_CASE("Native half FMA absolute source clears each half sign before broadcast", "[native-shader]") {
    for (bool broadcast : {false, true}) {
        const std::array<Native, 2> input{{{{0x1a087231, 0x20000008,
            broadcast ? 0x00040a0dU : 0x0004020dU, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        ScalarNativeMachine machine;
        machine.regs[26] = 0xc400c000;
        machine.regs[8] = 0x40003c00;
        machine.regs[13] = 0x44004200;
        machine.Execute(block);
        REQUIRE(machine.regs[8] == (broadcast ? 0x45004500U : 0x47004500U));
    }
}

TEST_CASE("Native constant interpolation retains attribute and flat input behavior", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00047326, 255, 0x000e403c, 0}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(((lowered.instructions[0] >> 54) & 3) == 2);
    class FlatEnvironment : public EmptyEnvironment {
    public:
        FlatEnvironment() { sph.ps.imap_generic_vector[7].raw = 0x55; }
    } env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    visitor.IPA(lowered.instructions[0]);
    u32 reads{};
    for (const auto& inst : block.Instructions()) {
        REQUIRE(inst.GetOpcode() != IR::Opcode::FPMul32);
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            REQUIRE(inst.Arg(0).Attribute() == IR::Attribute{60});
            ++reads;
        }
    }
    REQUIRE(reads == 1);
}

TEST_CASE("Native uniform predicate comparisons preserve all seven predicates and accumulation", "[native-shader]") {
    for (u32 output = 0; output < 7; ++output) {
        for (u32 combine = 0; combine < 3; ++combine) {
            for (bool invert : {false, true}) {
                std::array<Native, 2> input{{{{0x0700788c, 1, 0x0bfa5070, 0}}, VertexTransform.back()}};
                auto& w = input[0].words[2];
                w = (w & ~((7U << 17) | (15U << 23) | (3U << 10))) |
                    (output << 17) | (output << 23) | (u32{invert} << 26) | (combine << 10);
                Shader::ObjectPool<IR::Inst> pool;
                IR::Block block{pool};
                TranslateScalarNative(input, block);
                for (u32 bank : {0U, 0x2aU, 0x55U, 0x7fU}) {
                    for (u32 value : {0U, 1U, 2U, 0xffffffffU}) {
                        ScalarNativeMachine machine;
                        machine.regs[223] = value;
                        machine.regs[239] = bank;
                        for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (p & 1) != 0;
                        const auto original = machine.predicates;
                        machine.Execute(block);
                        const bool a = value != 1;
                        const bool b = ((bank & (1U << (6 - output))) != 0) != invert;
                        const bool expected = combine == 0 ? a && b : combine == 1 ? a || b : a != b;
                        const u32 mask = 1U << (6 - output);
                        REQUIRE(machine.regs[239] == ((bank & ~mask) | (expected ? mask : 0U)));
                        REQUIRE(machine.predicates == original);
                        REQUIRE(machine.regs[223] == value);
                    }
                }
            }
        }
    }
}

TEST_CASE("Native uniform predicate LUT and ordinary copies keep separate banks", "[native-shader]") {
    for (u32 output = 0; output < 7; ++output) {
        for (u32 lut : {0x80U, 0x40U}) {
            std::array<Native, 2> input{{{{0x0000789c, 0, 0x00fce820, 0}}, VertexTransform.back()}};
            auto& w = input[0].words[2];
            w = (w & ~((7U << 17) | 7U | (31U << 8))) | (output << 17) | (lut & 7) | ((lut >> 3) << 8);
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            TranslateScalarNative(input, block);
            for (u32 bank = 0; bank < 128; ++bank) {
                ScalarNativeMachine machine;
                machine.regs[239] = bank;
                for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (p & 1) != 0;
                const auto original = machine.predicates;
                machine.Execute(block);
                const bool expected = (bank & (1U << 5)) && (((bank & (1U << 4)) != 0) != (lut == 0x40));
                const u32 mask = 1U << (6 - output);
                REQUIRE(machine.regs[239] == ((bank & ~mask) | (expected ? mask : 0U)));
                REQUIRE(machine.predicates == original);
            }
        }
    }
    for (u32 source = 0; source < 7; ++source) {
        for (bool invert : {false, true}) {
            std::array<Native, 2> input{{{{0x0000781c, 0, 0x03f4f058, 0}}, VertexTransform.back()}};
            auto& w = input[0].words[2];
            const u32 lut = invert ? 0x40U : 0x80U;
            w = (w & ~((15U << 4) | 7U | (31U << 8))) | (source << 4) | (lut & 7) | ((lut >> 3) << 8);
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            TranslateScalarNative(input, block);
            for (u32 bank : {0U, 0x2aU, 0x55U, 0x7fU}) {
                ScalarNativeMachine machine;
                machine.regs[239] = bank;
                machine.predicates.fill(true);
                machine.Execute(block);
                REQUIRE(machine.predicates[2] == (((bank & (1U << (6 - source))) != 0) != invert));
                REQUIRE(machine.regs[239] == bank);
                for (u32 p = 0; p < 7; ++p) if (p != 2) REQUIRE(machine.predicates[p]);
            }
        }
    }
}

TEST_CASE("Native predicated uniform address arithmetic uses the selected uniform predicate", "[native-shader]") {
    const std::array<Native, 2> unsupported{{{{0x0405f891, 1, 0x0f8e103f, 0}}, VertexTransform.back()}};
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unsupported));
    for (u32 condition = 0; condition < 7; ++condition) {
        for (bool invert : {false, true}) {
            std::array<Native, 2> input{{{{0x04057891, 1, 0x0f8e103f, 0}}, VertexTransform.back()}};
            input[0].words[0] = (input[0].words[0] & ~0xf000U) | ((condition | (invert ? 8U : 0U)) << 12);
            const auto lowered = Shader::Ampere::LowerToMaxwell(input);
            REQUIRE(lowered);
            // The scalar evaluator ignores execution predicates, so check the real emitted guards separately.
            REQUIRE(((lowered.instructions[4] >> 16) & 15) == (6U | (invert ? 8U : 0U)));
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            const std::span prefix{lowered.instructions.data(), size_t{4}};
            EmptyEnvironment env;
            Shader::Maxwell::TranslatorVisitor visitor{env, block};
            visitor.P2R_imm(prefix[0]);
            visitor.SHR_imm(prefix[1]);
            visitor.SHL_imm(prefix[2]);
            visitor.R2P_imm(prefix[3]);
            for (u32 bank : {0U, 0x2aU, 0x55U, 0x7fU}) {
                ScalarNativeMachine machine;
                machine.regs[239] = bank;
                machine.predicates.fill(true);
                machine.Execute(block);
                REQUIRE(machine.predicates[6] == ((bank & (1U << (6 - condition))) != 0));
                REQUIRE(machine.regs[239] == bank);
            }
        }
    }
}

TEST_CASE("Native unsigned clamp tracks the selected uniform predicate", "[native-shader]") {
    for (u32 condition = 0; condition < 7; ++condition) {
        std::array<Native, 3> input{{
            {{0x0400788c, 2, 0x0bf01070, 0}},
            {{0x04047287, 2, 0x08000000, 0}},
            VertexTransform.back(),
        }};
        input[1].words[0] = (input[1].words[0] & ~0xfffU) | 0x887;
        input[0].words[2] |= condition << 17;
        input[1].words[2] |= condition << 23;
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        REQUIRE(lowered);
        REQUIRE(Shader::Maxwell::Decode(lowered.instructions[lowered.source_instruction_offsets[1]]) ==
                Shader::Maxwell::Opcode::IMNMX_imm);
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 2U, 3U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[220] = value;
            machine.regs[239] = 0x55;
            machine.predicates.fill(true);
            machine.Execute(block);
            REQUIRE(machine.regs[220] == std::min(value, 2U));
            for (bool pred : machine.predicates) REQUIRE(pred);
        }
        input[1].words[2] ^= 1U << 23;
        if (condition != 6) {
            const auto mismatch = Shader::Ampere::LowerToMaxwell(input);
            REQUIRE(mismatch);
            REQUIRE(Shader::Maxwell::Decode(mismatch.instructions[mismatch.source_instruction_offsets[1]]) !=
                    Shader::Maxwell::Opcode::IMNMX_imm);
        }
    }
}

TEST_CASE("Native predicate LUT masks its inverted middle input", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0000781c, 0, 0x01fac470, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bits = 0; bits < 128; ++bits) {
        ScalarNativeMachine machine;
        for (u32 p = 0; p < 7; ++p) machine.predicates[p] = ((bits >> p) & 1) != 0;
        auto expected = machine.predicates;
        expected[5] = expected[3] && !expected[6];
        machine.Execute(block);
        REQUIRE(machine.predicates == expected);
    }
}

TEST_CASE("Native cancellation retains its enclosing scopes at a parent join", "[native-shader]") {
    std::array<Native, 14> input{{
        {{0x27945, 192, 0x03800000, 0}},
        {{0x17945, 160, 0x03800000, 0}},
        {{0x7945, 128, 0x03800000, 0}},
        {{0x37945, 64, 0x03800000, 0}},
        {{0x38942, 0, 0x03800000, 0}},
        {{0x8947, 64, 0x03800000, 0}},
        {{0x27802, 1, 0xf00, 0}},
        {{0x37941, 0, 0x03800000, 0}},
        {{0x27802, 3, 0xf00, 0}},
        {{0x7918, 0, 0, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        {{0x17941, 0, 0x03800000, 0}},
        {{0x27941, 0, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto broken = input;
    broken[5].words[1] = 48;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[4].words[0] = 0x18942;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class BarrierEnvironment final : public EmptyEnvironment {
    public:
        explicit BarrierEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const u32 offset = lowered.source_instruction_offsets[8];
    const Shader::Maxwell::Location continuation{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    for (bool predicate : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool visited_continuation = false, exited = false;
        std::string trace;
        for (u32 step = 0; step < 64 && block; ++step) {
            trace += std::to_string(block->begin.Offset()) + ":" + std::to_string(block->end.Offset()) + " ";
            visited_continuation |= !block->begin.IsVirtual() && block->Contains(continuation);
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            const bool taken = (pred == IR::Pred::PT || predicate) != inverted;
            block = taken ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        INFO("path=" << trace << " marker=" << continuation.Offset());
        REQUIRE(visited_continuation == predicate);
    }
}

TEST_CASE("Native cube array shadow sample preserves layer XYZ reference and destination", "[native-shader]") {
    std::array<Native, 2> input{{{{0x20217b60, 0xe0801a44, 0x001e41ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    std::vector<u32> reads;
    u32 samples{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetRegister) reads.push_back(static_cast<u32>(inst.Arg(0).Reg()));
        if (inst.GetOpcode() == IR::Opcode::BoundImageSampleDrefImplicitLod) {
            REQUIRE(inst.Arg(0).U32() == 104);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorArrayCube);
            REQUIRE(inst.Flags<IR::TextureInstInfo>().is_depth.Value() == 1);
            REQUIRE(inst.Arg(2).Inst()->Arg(0).Inst()->Arg(0).Reg() == IR::Reg{68});
            ++samples;
        }
    }
    REQUIRE(samples == 1);
    std::sort(reads.begin(), reads.end());
    REQUIRE(reads == std::vector<u32>{32, 33, 34, 35, 68});
    REQUIRE(block.Instructions().back().Arg(0).Reg() == IR::Reg{33});
    input[0].words[1] |= 1U << 28;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
}

TEST_CASE("Native computed entry cannot bypass the sync before a warp memory barrier", "[native-shader]") {
    std::vector<u64> code(64);
    const std::array<u32, 4> offsets{0, 2, 4, 8};
    code[0x50 / 8] = 0xf0f800000007000fULL;
    code[0x68 / 8] = 0xef98000000070100ULL;
    Shader::Ampere::AppendNativeAddressMap(code, offsets);
    REQUIRE(Shader::Ampere::NativeProgramAddress(code, 0) == 0x50U);
    REQUIRE_FALSE(Shader::Ampere::NativeProgramAddress(code, 16));
    REQUIRE(Shader::Ampere::NativeProgramAddress(code, 32) == 0x78U);
    const auto restored = code;
    REQUIRE_FALSE(Shader::Ampere::NativeProgramAddress(restored, 16));
}

TEST_CASE("Native float predicate comparisons retain uniform values and source modifiers", "[native-shader]") {
    const bool absolute = GENERATE(false, true);
    const bool negate = GENERATE(false, true);
    for (u32 source : {0U, 5U, 15U, 16U, 17U, 63U}) {
        std::array<Native, 2> input{{{{0xff007c0b, source, 0x0bf53000, 0}}, VertexTransform.back()}};
        input[0].words[1] |= (u32{absolute} << 30) | (u32{negate} << 31);
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (float value : {-2.0f, -0.0f, 0.0f, 3.0f, std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            ScalarNativeMachine machine;
            if (source != 63) machine.regs[216 + source] = std::bit_cast<u32>(value);
            for (u32 p = 0; p < 7; ++p) machine.predicates[p] = (p & 1) != 0;
            auto expected = machine.predicates;
            float operand = source == 63 ? 0.0f : value;
            if (absolute) operand = std::abs(operand);
            if (negate) operand = -operand;
            expected[2] = 0.0f <= operand;
            machine.Execute(block);
            REQUIRE(machine.predicates == expected);
            if (source != 63) REQUIRE(machine.regs[216 + source] == std::bit_cast<u32>(value));
        }
        input[0].words[1] = 18;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    }
}

TEST_CASE("Native fused multiply add retains its uniform multiplier and aliased accumulator", "[native-shader]") {
    const bool saturate = GENERATE(false, true);
    std::array<Native, 2> input{{{{0x05127c23, 8, 0x08012012, 0}}, VertexTransform.back()}};
    if (!saturate) input[0].words[2] &= ~(1U << 13);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (const auto& values : std::array<std::array<float, 3>, 5>{{
        {{-1, 3, 2}}, {{3, 2, 0}}, {{0.25f, 2, 0.25f}}, {{-2, -3, -5}},
        {{1.0f + 0x1p-23f, 1.0f - 0x1p-23f, -1.0f}},
    }}) {
        ScalarNativeMachine machine;
        machine.regs[5] = std::bit_cast<u32>(values[0]);
        machine.regs[224] = std::bit_cast<u32>(values[1]);
        machine.regs[18] = std::bit_cast<u32>(values[2]);
        machine.predicates.fill(true);
        float expected = std::fma(values[0], values[1], values[2]);
        if (saturate) expected = std::clamp(expected, 0.0f, 1.0f);
        machine.Execute(block);
        REQUIRE(machine.regs[18] == std::bit_cast<u32>(expected));
        REQUIRE(machine.regs[5] == std::bit_cast<u32>(values[0]));
        REQUIRE(machine.regs[224] == std::bit_cast<u32>(values[1]));
        for (bool p : machine.predicates) REQUIRE(p);
    }
}

TEST_CASE("Native high LEA retains its constant base and high scaled word", "[native-shader]") {
    const u32 shift = GENERATE(0U, 1U, 16U, 30U, 31U);
    for (u32 destination : {5U, 7U}) {
        std::array<Native, 2> input{{{{0x05077a11, 0x00c00000, 0x078ff0ff, 0}}, VertexTransform.back()}};
        input[0].words[0] = (input[0].words[0] & ~0xff0000U) | (destination << 16);
        input[0].words[2] = (input[0].words[2] & ~(31U << 11)) | (shift << 11);
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
            for (u32 base : {0U, 1U, 0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[5] = value;
                machine.constants[u64{3} << 32] = base;
                machine.predicates.fill(true);
                machine.Execute(block);
                REQUIRE(machine.regs[destination] == static_cast<u32>((u64{value} << shift) >> 32) + base);
                if (destination != 5) REQUIRE(machine.regs[5] == value);
                for (bool pred : machine.predicates) REQUIRE(pred);
            }
        }
        input[0].words[2] &= ~(7U << 17);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    }
}

TEST_CASE("Native capture treats predicated NOINC forward exits as branches", "[native-shader]") {
    using Capture = Shader::Ampere::ProgramCaptureState;
    Capture loop;
    REQUIRE(loop.Observe(Native{{0x3944, 16, 0x03c00000, 0}}, 0) == Capture::Result::Continue);
    REQUIRE(loop.Observe(Native{{0x7947, 0xffffffe0, 0x0383ffff, 0}}, 1) == Capture::Result::Continue);
    REQUIRE(loop.Observe(VertexTransform.back(), 2) == Capture::Result::Complete);
    Capture forward;
    REQUIRE(forward.Observe(Native{{0x3944, 48, 0x03c00000, 0}}, 0) == Capture::Result::Continue);
    REQUIRE(forward.Observe(VertexTransform.back(), 1) == Capture::Result::Continue);
    REQUIRE(forward.Observe(VertexTransform.back(), 4) == Capture::Result::Complete);
}

TEST_CASE("Native uniform LUT preserves its bank and aliases its source", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x04047892, 0x3fffffff, 0x0f8ec03f, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x3fffffffU, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[220] = value;
        machine.regs[239] = 0x55;
        machine.predicates.fill(true);
        machine.Execute(block);
        REQUIRE(machine.regs[220] == (value & 0x3fffffffU));
        REQUIRE(machine.regs[239] == 0x55);
        for (bool p : machine.predicates) REQUIRE(p);
    }
}

TEST_CASE("Native unsigned conversion toward zero is exact at float precision boundaries", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00077245, 2, 0x0020d000, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    const std::array<std::pair<u32, u32>, 10> cases{{
        {0, 0}, {1, 0x3f800000}, {0x00ffffff, 0x4b7fffff}, {0x01000000, 0x4b800000},
        {0x01000001, 0x4b800000}, {0x01000003, 0x4b800001}, {0x7fffffff, 0x4effffff},
        {0x80000000, 0x4f000000}, {0x800000ff, 0x4f000000}, {0xffffffff, 0x4f7fffff},
    }};
    for (const auto& [source, expected] : cases) {
        ScalarNativeMachine machine;
        machine.regs[2] = source;
        machine.predicates.fill(true);
        machine.Execute(block);
        REQUIRE(machine.regs[7] == expected);
        REQUIRE(machine.regs[2] == source);
        for (bool p : machine.predicates) REQUIRE(p);
    }
}

TEST_CASE("Native scalar uniform add preserves wraparound and its source", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00147c10, 4, 0x0fffe0ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 a : {0U, 1U, 0xffffffffU}) for (u32 b : {0U, 1U, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[0] = a;
        machine.regs[220] = b;
        machine.Execute(block);
        REQUIRE(machine.regs[20] == a + b);
        REQUIRE(machine.regs[0] == a);
        REQUIRE(machine.regs[220] == b);
    }
}

TEST_CASE("Native scalar strong global loads retain device memory ordering", "[native-shader]") {
    const bool strong = GENERATE(false, true);
    const std::array<Native, 2> input{{{{0x14067381, 0, strong ? 0x001ea900U : 0x001e8900U, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[20] = 0x1000;
    machine.regs[21] = 0x22;
    machine.global[0x2200001000ULL] = 0xdeadbeef;
    machine.Execute(block);
    REQUIRE(machine.regs[6] == 0xdeadbeef);
    REQUIRE(machine.device_barriers == (strong ? 2U : 0U));
}

TEST_CASE("Native three source addition keeps negative immediates and negated third inputs", "[native-shader]") {
    const bool negate_c = GENERATE(false, true);
    for (u32 immediate : {0U, 33U, 0xffffff82U}) {
        std::array<Native, 2> input{{{{0x232b7810, immediate, 0x07ffe024, 0}}, VertexTransform.back()}};
        input[0].words[2] |= u32{negate_c} << 11;
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : {0U, 1U, 0x80000000U, 0xffffffffU}) for (u32 c : {0U, 1U, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[35] = a;
            machine.regs[36] = c;
            machine.Execute(block);
            REQUIRE(machine.regs[43] == a + immediate + (negate_c ? 0U - c : c));
            REQUIRE(machine.regs[35] == a);
            REQUIRE(machine.regs[36] == c);
        }
    }
}

TEST_CASE("Native permutation sign extends the low half without changing its source", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x3a377816, 0x9910, 0xff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x7fffU, 0x8000U, 0xffffU, 0xdead8000U, 0xabcd7fffU}) {
        ScalarNativeMachine machine;
        machine.regs[58] = value;
        machine.Execute(block);
        REQUIRE(machine.regs[55] == static_cast<u32>(static_cast<s32>(static_cast<s16>(value))));
        REQUIRE(machine.regs[58] == value);
    }
}

TEST_CASE("Native byte float conversions select all four bytes", "[native-shader]") {
    for (u32 byte = 0; byte < 4; ++byte) {
        const std::array<Native, 2> input{{{{0x003b7306, 33U | (byte << 28), 0x1000, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        ScalarNativeMachine machine;
        machine.regs[33] = 0xff807f00;
        machine.Execute(block);
        REQUIRE(machine.regs[59] == std::bit_cast<u32>(static_cast<float>((0xff807f00U >> (byte * 8)) & 255U)));
        REQUIRE(machine.regs[33] == 0xff807f00);
    }
}

TEST_CASE("Native predicate LUT negates both first inputs", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0000781c, 0, 0x027c0072, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bits = 0; bits < 128; ++bits) {
        ScalarNativeMachine machine;
        for (u32 p = 0; p < 7; ++p) machine.predicates[p] = ((bits >> p) & 1) != 0;
        auto expected = machine.predicates;
        expected[6] = !expected[4] && !expected[0];
        machine.Execute(block);
        REQUIRE(machine.predicates == expected);
    }
}

TEST_CASE("Native uniform moves read the zero register without aliasing the bank", "[native-shader]") {
    for (u32 source : {0U, 5U, 15U, 16U, 17U, 63U}) {
        const std::array<Native, 2> input{{{{0x00047c82, source, 0x08000000, 0}}, VertexTransform.back()}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        REQUIRE(lowered);
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        EmptyEnvironment env;
        Shader::Maxwell::TranslatorVisitor visitor{env, block};
        visitor.MOV_reg(lowered.instructions[0]);
        ScalarNativeMachine machine;
        machine.regs.fill(0xa5a5a5a5);
        machine.Execute(block);
        REQUIRE(machine.regs[220] == (source == 63 ? 0U : 0xa5a5a5a5U));
        REQUIRE(machine.regs[239] == 0xa5a5a5a5U);
    }
    for (u32 source : {18U, 62U, 64U, 255U}) {
        const std::array<Native, 2> input{{{{0x00047c82, source, 0x08000000, 0}}, VertexTransform.back()}};
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    }
}

TEST_CASE("Native LUT30 clears selected bits and preserves aliased sources", "[native-shader]") {
    for (u32 destination : {0U, 1U, 3U}) {
        const std::array<Native, 2> input{{{{0x00007212U | (destination << 16), 1, 0x078e3002, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : {0U, 1U, 0xaaaaaaaaU, 0xffffffffU})
            for (u32 b : {0U, 1U, 0x55555555U, 0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[0] = a; machine.regs[1] = b; machine.regs[2] = 0xffffffff;
                machine.predicates.fill(true);
                machine.Execute(block);
                REQUIRE(machine.regs[destination] == (a & ~b));
                for (bool predicate : machine.predicates) REQUIRE(predicate);
            }
    }
}

TEST_CASE("Native uniform logical right shift retains unsigned high bits", "[native-shader]") {
    for (u32 shift : {0U, 1U, 2U, 16U, 31U, 32U}) {
        const std::array<Native, 2> input{{{{0x3f057899, shift, 0x08011604, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[220] = value;
            machine.Execute(block);
            REQUIRE(machine.regs[221] == (shift < 32 ? value >> shift : 0U));
            REQUIRE(machine.regs[220] == value);
        }
    }
}

TEST_CASE("Native uniform permutation sign extends its low half and supports aliasing", "[native-shader]") {
    for (u32 destination : {6U, 8U}) {
        const std::array<Native, 2> input{{{{0x08007896U | (destination << 16), 0x9910, 0x0800003f, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 0x7fffU, 0x8000U, 0xffffU, 0xdead8000U, 0xabcd7fffU}) {
            ScalarNativeMachine machine;
            machine.regs[224] = value;
            machine.Execute(block);
            REQUIRE(machine.regs[destination + 216] == static_cast<u32>(static_cast<s32>(static_cast<s16>(value))));
            if (destination != 8) REQUIRE(machine.regs[224] == value);
        }
    }
}

TEST_CASE("Native uniform bit tests update one uniform predicate and preserve scalar predicates", "[native-shader]") {
    const u32 bit_mask = GENERATE(0U, 1U, 0x20000U, 0x40000U, 0xffffffffU);
    for (u32 output = 0; output < 7; ++output) {
        const std::array<Native, 2> input{{{{0x0a3f7892, bit_mask, 0x0f80c03fU | (output << 17), 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 2U, 0x20000U, 0x40000U, 0x80000000U, 0xffffffffU})
            for (u32 bank : {0U, 0x55U, 0x7fU}) {
                ScalarNativeMachine machine;
                machine.regs[226] = value; machine.regs[239] = bank;
                machine.predicates = {true, false, true, false, true, false, true};
                const auto original = machine.predicates;
                machine.Execute(block);
                const u32 mask = 1U << (6 - output);
                REQUIRE(machine.regs[239] == ((bank & ~mask) | ((value & bit_mask) ? mask : 0U)));
                REQUIRE(machine.predicates == original);
                REQUIRE(machine.regs[226] == value);
            }
    }
}

TEST_CASE("Native uniform register effective addresses preserve scaled addition and aliasing", "[native-shader]") {
    for (u32 shift : {0U, 1U, 2U, 16U, 31U}) {
        const std::array<Native, 2> input{{{{0x05047291, 8, 0x0f8e003fU | (shift << 11), 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[221] = value; machine.regs[224] = 0xfffffff0;
            machine.Execute(block);
            REQUIRE(machine.regs[220] == ((value << shift) + 0xfffffff0U));
            REQUIRE(machine.regs[221] == value);
            REQUIRE(machine.regs[224] == 0xfffffff0U);
        }
    }
}

TEST_CASE("Native uniform global reduction preserves its full pointer and wrapping addend", "[native-shader]") {
    const std::array<Native, 2> input{{{{0xff00798e, 0, 0x0c10e104, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 low : {0U, 0xfffffff0U, 0xfffffffcU}) {
        ScalarNativeMachine machine;
        machine.regs[220] = low; machine.regs[221] = 0x21; machine.regs[0] = 17;
        const u64 address = 0x2100000000ULL | low;
        machine.global[address] = 0xfffffff8;
        machine.Execute(block);
        REQUIRE(machine.global == std::unordered_map<u64, u32>{{address, 9}});
        REQUIRE(machine.regs[220] == low);
        REQUIRE(machine.regs[221] == 0x21);
        REQUIRE(machine.regs[0] == 17);
    }
}

TEST_CASE("Native XOR and inverted outer-input LUTs retain data and predicate truth tables", "[native-shader]") {
    for (u32 lut : {4U, 0x0cU, 0x3cU}) {
        for (u32 destination : {0U, 1U, 2U, 3U}) {
            const std::array<Native, 2> input{{{{0x00007212U | (destination << 16), 1, 0x07820002U | (lut << 8), 0}}, VertexTransform.back()}};
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            TranslateScalarNative(input, block);
            for (u32 a : {0U, 1U, 0xaaaaaaaaU, 0xffffffffU})
                for (u32 b : {0U, 1U, 0x55555555U, 0xffffffffU})
                    for (u32 c : {0U, 1U, 0xccccccccU, 0xffffffffU}) {
                        ScalarNativeMachine machine;
                        machine.regs[0] = a; machine.regs[1] = b; machine.regs[2] = c;
                        machine.predicates.fill(true);
                        machine.Execute(block);
                        const u32 expected = lut == 4 ? (~a & b & ~c) : lut == 0x0c ? (~a & b) : (a ^ b);
                        REQUIRE(machine.regs[destination] == expected);
                        REQUIRE(machine.predicates[1] == (expected != 0));
                        for (u32 p = 0; p < 7; ++p) if (p != 1) REQUIRE(machine.predicates[p]);
                    }
        }
    }
}

TEST_CASE("Native uniform carry chains preserve 64-bit overflow and both predicate banks", "[native-shader]") {
    for (u32 output = 0; output < 7; ++output) {
        const std::array<Native, 3> input{{
            {{0x04047290, 8, 0x0ff1e03fU | (output << 17), 0}},
            {{0x3f057290, 9, 0x087fe43fU | (output << 23), 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : {0U, 1U, 0xfffffff0U, 0xffffffffU})
            for (u32 b : {0U, 1U, 32U, 0xffffffffU})
                for (u32 high : {0U, 0xffffffffU}) {
                    ScalarNativeMachine machine;
                    machine.regs[220] = a; machine.regs[224] = b; machine.regs[225] = high;
                    machine.regs[239] = 0x55;
                    machine.predicates = {true, false, true, false, true, false, true};
                    const auto predicates = machine.predicates;
                    machine.Execute(block);
                    const u64 sum = u64{a} + b;
                    const u32 mask = 1U << (6 - output);
                    REQUIRE(machine.regs[220] == static_cast<u32>(sum));
                    REQUIRE(machine.regs[221] == high + static_cast<u32>(sum >> 32));
                    REQUIRE(machine.regs[239] == ((0x55U & ~mask) | ((sum >> 32) ? mask : 0U)));
                    REQUIRE(machine.predicates == predicates);
                    REQUIRE(machine.regs[224] == b);
                }
    }
}

TEST_CASE("Native upper uniform registers remain separate from predicate state", "[native-shader]") {
    const std::array<Native, 4> input{{
        {{0x00107882, 0xffffffff, 0, 0}},
        {{0x00117882, 0x12345678, 0, 0}},
        {{0x10107890, 2, 0x0fffe03f, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[239] = 0x55;
    machine.Execute(block);
    REQUIRE(machine.regs[232] == 1);
    REQUIRE(machine.regs[233] == 0x12345678);
    REQUIRE(machine.regs[239] == 0x55);
}

TEST_CASE("Native uniform predicate extraction selects UP0 without changing either bank", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x3f067883, 1, 0x08000000, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bank = 0; bank < 128; ++bank) {
        ScalarNativeMachine machine;
        machine.regs[239] = bank;
        machine.predicates.fill(true);
        machine.Execute(block);
        REQUIRE(machine.regs[222] == ((bank >> 6) & 1));
        REQUIRE(machine.regs[239] == bank);
        for (bool predicate : machine.predicates) REQUIRE(predicate);
    }
}

TEST_CASE("Native multi-scoreboard dependency waits retain surrounding dataflow", "[native-shader]") {
    const std::array<Native, 3> input{{{{0x00007802, 17, 0x00000f00, 0}},
        {{0x0000791a, 0x800e, 0, 0}}, VertexTransform.back()}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.Execute(block);
    REQUIRE(machine.regs[0] == 17);
}

TEST_CASE("Native scalar effective addresses add the selected uniform base", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x351c7c11, 5, 0x0f8e10ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[53] = value; machine.regs[221] = 0xfffffff0;
        machine.Execute(block);
        REQUIRE(machine.regs[28] == ((value << 2) + 0xfffffff0U));
        REQUIRE(machine.regs[53] == value);
        REQUIRE(machine.regs[221] == 0xfffffff0U);
    }
}

TEST_CASE("Native mixed predicate AND retains its scalar input and uniform bank", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x0000781c, 0, 0x00f4e848, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 bank = 0; bank < 128; ++bank)
        for (bool scalar : {false, true}) {
            ScalarNativeMachine machine;
            machine.regs[239] = bank;
            machine.predicates.fill(true); machine.predicates[1] = scalar;
            auto expected = machine.predicates;
            expected[2] = scalar && !(bank & (1U << 2));
            machine.Execute(block);
            REQUIRE(machine.predicates == expected);
            REQUIRE(machine.regs[239] == bank);
        }
}

TEST_CASE("Native uniform shifts honor their uniform execution predicate", "[native-shader]") {
    for (u32 condition = 0; condition < 7; ++condition)
        for (bool invert : {false, true}) {
            const std::array<Native, 2> input{{{{0x3f070899U | ((condition | (invert ? 8U : 0U)) << 12), 2, 0x08011606, 0}}, VertexTransform.back()}};
            const auto lowered = Shader::Ampere::LowerToMaxwell(input);
            REQUIRE(lowered);
            REQUIRE(((lowered.instructions[4] >> 16) & 15) == (6U | (invert ? 8U : 0U)));
            REQUIRE(Shader::Maxwell::Decode(lowered.instructions[4]) == Shader::Maxwell::Opcode::SHR_imm);
            REQUIRE(Shader::Maxwell::Decode(lowered.instructions[5]) == Shader::Maxwell::Opcode::R2P_imm);
        }
}

TEST_CASE("Native shared loads retain uniform offsets and their byte displacement", "[native-shader]") {
    const std::array<Native, 2> input{{{{0xff1c7984, 0x13009, 0x08000800, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 address : {0U, 4U, 128U}) {
        ScalarNativeMachine machine;
        machine.regs[225] = address;
        machine.shared[address + 0x130] = 0x12345678;
        machine.Execute(block);
        REQUIRE(machine.regs[28] == 0x12345678);
        REQUIRE(machine.regs[225] == address);
    }
}

TEST_CASE("Native uniform shared stores and loads retain their data and displacement", "[native-shader]") {
    const std::array<Native, 3> input{{{{0xff007988, 0x13020, 0x08000809, 0}},
        {{0xff1c7984, 0x13009, 0x08000800, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[225] = 64; machine.regs[32] = 0x12345678;
    machine.Execute(block);
    REQUIRE(machine.shared == std::unordered_map<u64, u32>{{64 + 0x130, 0x12345678}});
    REQUIRE(machine.regs[28] == 0x12345678);
    REQUIRE(machine.regs[225] == 64);
}

TEST_CASE("Native constant signed minmax retains negative bounds and execution predicates", "[native-shader]") {
    const std::array<Native, 2> input{{{{0xff017a17, 0xa00, 0x07800200, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (s32 value : {-10, -1, 0, 1, 100}) {
        ScalarNativeMachine machine;
        machine.constants[0x28] = static_cast<u32>(value);
        machine.Execute(block);
        REQUIRE(machine.regs[1] == static_cast<u32>(std::max(0, value)));
    }
    auto predicated = input;
    predicated[0].words[0] = 0xff01aa17;
    const auto lowered = Shader::Ampere::LowerToMaxwell(predicated);
    REQUIRE(lowered);
    for (u64 instruction : std::span{lowered.instructions}.first(lowered.instructions.size() - 1))
        REQUIRE(((instruction >> 16) & 15) == 10);
}

TEST_CASE("Native wide global stores preserve all words across a low-address carry", "[native-shader]") {
    for (u32 width : {4U, 5U, 6U}) {
        const std::array<Native, 2> input{{{{0x00007386, 12, 0x0010a100U | (width << 9), 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        ScalarNativeMachine machine;
        machine.regs[0] = 0xfffffffc; machine.regs[1] = 0x21;
        const u32 words = 1U << (width - 4);
        for (u32 i = 0; i < words; ++i) machine.regs[12 + i] = 0x12345678 + i;
        machine.Execute(block);
        REQUIRE(machine.global.size() == words);
        for (u32 i = 0; i < words; ++i)
            REQUIRE(machine.global.at(0x21fffffffcULL + i * 4) == 0x12345678 + i);
    }
}

TEST_CASE("Native split cancellation visits only its selected cleanup path", "[native-shader]") {
    std::array<Native, 14> input{{
        {{0x7945, 192, 0x03800000, 0}},
        {{0x17945, 80, 0x03800000, 0}},
        {{0x10942, 0, 0x03800000, 0}},
        {{0x8947, 32, 0x03800000, 0}},
        {{0x7947, 64, 0x03800000, 0}},
        {{0x27802, 99, 0xf00, 0}},
        {{0x17941, 0, 0x03800000, 0}},
        {{0x27802, 2, 0xf00, 0}},
        {{0x7947, 48, 0x03800000, 0}},
        {{0x27802, 3, 0xf00, 0}},
        {{0x7947, 16, 0x03800000, 0}},
        {{0x27802, 99, 0xf00, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto broken = input;
    broken[3].words[0] ^= 0x8000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[10].words[1] = static_cast<u32>(-32);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class BarrierEnvironment final : public EmptyEnvironment {
    public:
        explicit BarrierEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const u32 offset = lowered.source_instruction_offsets[9];
    const Shader::Maxwell::Location continuation{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    for (bool predicate : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool visited_continuation = false, exited = false;
        std::string trace;
        for (u32 step = 0; step < 64 && block; ++step) {
            trace += std::to_string(block->begin.Offset()) + ":" + std::to_string(block->end.Offset()) + " ";
            visited_continuation |= !block->begin.IsVirtual() && block->Contains(continuation);
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            const bool taken = (pred == IR::Pred::PT || predicate) != inverted;
            block = taken ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        INFO("path=" << trace << " marker=" << continuation.Offset());
        REQUIRE(visited_continuation == predicate);
    }
}

TEST_CASE("Native split cancellation proves a forward cleanup after intervening data", "[native-shader]") {
    std::array<Native, 14> input{{
        {{0x7945, 192, 0x03800000, 0}},
        {{0x17945, 80, 0x03800000, 0}},
        {{0x10942, 0, 0x03800000, 0}},
        {{0x8947, 32, 0x03800000, 0}},
        {{0x27802, 1, 0xf00, 0}},
        {{0x7947, 48, 0x03800000, 0}},
        {{0x17941, 0, 0x03800000, 0}},
        {{0x27802, 2, 0xf00, 0}},
        {{0x7947, 48, 0x03800000, 0}},
        {{0x27802, 3, 0xf00, 0}},
        {{0x7947, 16, 0x03800000, 0}},
        {{0x27802, 99, 0xf00, 0}},
        {{0x7941, 0, 0x03800000, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    auto broken = input;
    broken[3].words[0] ^= 0x8000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    broken = input;
    broken[10].words[1] = static_cast<u32>(-32);
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(broken));
    std::vector<u64> code(0x50 / 8);
    for (const auto word : lowered.instructions) {
        if ((code.size() * 8) % 32 == 0) code.push_back(0);
        code.push_back(word);
    }
    class BarrierEnvironment final : public EmptyEnvironment {
    public:
        explicit BarrierEnvironment(const std::vector<u64>& code_) : code{code_} { stage = Shader::Stage::VertexB; }
        u64 ReadInstruction(u32 address) override { return code.at(address / 8); }
        const std::vector<u64>& code;
    } env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    INFO(cfg.Dot());
    const u32 offset = lowered.source_instruction_offsets[9];
    const Shader::Maxwell::Location continuation{0x50 + offset * 8 + ((offset + 1) / 3) * 8};
    for (bool predicate : {false, true}) {
        const auto* block = &*cfg.Functions().front().blocks.begin();
        bool visited_continuation = false, exited = false;
        std::string trace;
        for (u32 step = 0; step < 64 && block; ++step) {
            trace += std::to_string(block->begin.Offset()) + ":" + std::to_string(block->end.Offset()) + " ";
            visited_continuation |= !block->begin.IsVirtual() && block->Contains(continuation);
            if (block->end_class == Shader::Maxwell::Flow::EndClass::Exit) { exited = true; break; }
            const auto [pred, inverted] = block->cond.GetPred();
            REQUIRE(block->cond.GetFlowTest() == IR::FlowTest::T);
            REQUIRE((pred == IR::Pred::P0 || pred == IR::Pred::PT));
            const bool taken = (pred == IR::Pred::PT || predicate) != inverted;
            block = taken ? block->branch_true : block->branch_false;
        }
        REQUIRE(exited);
        INFO("path=" << trace << " marker=" << continuation.Offset());
        REQUIRE(visited_continuation == predicate);
    }
}

TEST_CASE("Native byte permutation inserts the low source byte without clobbering aliases", "[native-shader]") {
    for (u32 destination : {0U, 1U, 2U}) {
        const std::array<Native, 2> input{{{{0x00007816U | (destination << 16), 0x7054, 1, 0}}, VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : {0U, 0x12345678U, 0xffffffffU})
            for (u32 b : {0U, 0x89abcdefU, 0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[0] = a; machine.regs[1] = b;
                machine.Execute(block);
                REQUIRE(machine.regs[destination] == ((b & 0xff00ffffU) | ((a & 0xffU) << 16)));
            }
    }
}

TEST_CASE("Native constant effective addresses allow an unused carry output", "[native-shader]") {
    const std::array<Native, 2> input{{{{0x00007a11, 0x01002e00, 0x078e40ff, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[0] = value;
        machine.constants[(u64{4} << 32) | 0xb8] = 0x12345678;
        machine.predicates.fill(true);
        machine.Execute(block);
        REQUIRE(machine.regs[0] == ((value << 8) + 0x12345678U));
        for (bool predicate : machine.predicates) REQUIRE(predicate);
    }
}
