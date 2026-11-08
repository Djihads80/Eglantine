// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
// SURED.D.2D.MAX.U64.STRONG.GPU.IGN [R16], R2, c[3][0].
constexpr Instruction LoggedMax{{0x1000759f, 0x60c00002, 0x0110e400, 0x0003e200}};

Instruction Move(u32 reg, u32 value) {
    return {{(reg << 16) | 0x7802, value, 0xf00, 0}};
}

class AtomicEnvironment final : public Shader::Environment {
public:
    explicit AtomicEnvironment(std::span<const u64> code_, Shader::Stage shader_stage = Shader::Stage::Compute) : code{code_} {
        stage = shader_stage;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return code[i];
        }
        throw std::out_of_range("Outside U64 atomic fixture");
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    // The instruction's explicit U64 type must win even with a non-integer descriptor.
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 3; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool IsNativeProgram() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
private:
    std::span<const u64> code;
};

void Save(std::span<const u32> words, const std::string& name) {
    if (const char* directory = std::getenv("EGLANTINE_U64_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        std::ofstream out{std::filesystem::path{directory} / name, std::ios::binary};
        out.write(reinterpret_cast<const char*>(words.data()),
                  static_cast<std::streamsize>(words.size_bytes()));
        REQUIRE(out.good());
    }
}
} // namespace

TEST_CASE("Ampere coverage reads become an input sample mask in fragment SPIRV", "[ampere][coverage][spirv]") {
    const Instruction coverage{{0x00017925, 0, 0x000e4000, 0}};
    const Instruction low{{0x00067a02, 3U << 22, 0xf00, 0}};
    const Instruction high{{0x00077a02, (3U << 22) | (4U << 6), 0xf00, 0}};
    const Instruction store{{0x06007386, 1, 0x00100900, 0}};
    const auto code = LowerToMaxwell(std::array{low, high, coverage, store, Exit});
    INFO(code.error);
    REQUIRE(static_cast<bool>(code));
    AtomicEnvironment env{code.instructions, Shader::Stage::Fragment};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::HostTranslateInfo host;
    host.support_int64 = true;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    CHECK(program.info.uses_sample_mask);
    CHECK_FALSE(program.info.uses_sample_id);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    bool mask = false;
    for (size_t off = 5; off < words.size(); off += words[off] >> 16)
        if ((words[off] & 65535) == static_cast<u32>(spv::Op::OpDecorate) &&
            words[off + 2] == static_cast<u32>(spv::Decoration::BuiltIn))
            mask |= words[off + 3] == static_cast<u32>(spv::BuiltIn::SampleMask);
    CHECK(mask);
    Save(words, "coverage.spv");
}

TEST_CASE("Ampere compare exchange preserves both operands and emits atomic SPIRV", "[ampere][cas][spirv]") {
    const Instruction cas{{0x060873a9, 0x0a, 0x001ee10b, 0}};
    const auto lowered = LowerToMaxwell(std::array{cas, Exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 4);
    CHECK(((lowered.instructions[0] >> 20) & 255) == 10);
    CHECK(((lowered.instructions[1] >> 20) & 255) == 11);
    CHECK(((lowered.instructions[2] >> 20) & 255) == 250);
    CHECK(((lowered.instructions[2] >> 8) & 255) == 6);
    CHECK((lowered.instructions[2] & 255) == 8);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    AtomicEnvironment env{lowered.instructions};
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (u64 operand : {254ULL, 255ULL}) {
        const u64 invalid = (lowered.instructions[2] & ~(255ULL << 20)) | (operand << 20);
        CHECK_THROWS_AS(visitor.ATOM_cas(invalid), Shader::NotImplementedException);
    }
    visitor.ATOM_cas(lowered.instructions[2]);
    bool found = false;
    for (auto& inst : block.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::GlobalAtomicCompareExchange32) continue;
        found = true;
        CHECK(inst.MayHaveSideEffects());
        const auto* pair = inst.Arg(1).InstRecursive();
        REQUIRE(pair->GetOpcode() == IR::Opcode::CompositeConstructU32x2);
        CHECK(pair->Arg(0).InstRecursive()->Arg(0).Reg() == IR::Reg::R250);
        CHECK(pair->Arg(1).InstRecursive()->Arg(0).Reg() == IR::Reg::R251);
    }
    REQUIRE(found);
    for (bool routed : {false, true}) {
        const Instruction low{{0x00067a02, 3U << 22, 0xf00, 0}};
        const Instruction high{{0x00077a02, (3U << 22) | (4U << 6), 0xf00, 0}};
        const auto code = LowerToMaxwell(std::array{low, high, Move(10, 0x12345678), Move(11, 0xabcdef01), cas, Exit});
        REQUIRE(static_cast<bool>(code));
        AtomicEnvironment fixture{code.instructions};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{fixture, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<IR::Inst> inst_pool;
        Shader::ObjectPool<IR::Block> block_pool;
        Shader::HostTranslateInfo host;
        host.support_int64 = true;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, fixture, cfg, host);
        u32 atomics = 0;
        for (auto* b : program.blocks) for (auto& inst : b->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::StorageAtomicCompareExchange32) continue;
            ++atomics;
            CHECK(inst.MayHaveSideEffects());
            if (routed) {
                const auto operands = inst.Arg(2);
                inst.SetArg(2, IR::Value{});
                inst.ReplaceOpcode(IR::Opcode::GlobalAtomicCompareExchange32);
                inst.SetArg(0, IR::Value{u64{0x1000}});
                inst.SetArg(1, operands);
                program.info.uses_global_memory = true;
                program.info.native_global_bounds = true;
            }
        }
        REQUIRE(atomics == 1);
        REQUIRE(program.info.storage_buffers_descriptors.size() == 1);
        CHECK(program.info.storage_buffers_descriptors[0].is_written);
        Shader::Optimization::CollectShaderInfoPass(fixture, program);
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.support_int64 = true;
        const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        u32 count = 0;
        for (size_t off = 5; off < words.size(); off += words[off] >> 16)
            count += (words[off] & 65535) == static_cast<u32>(spv::Op::OpAtomicCompareExchange);
        CHECK(count == 1);
        Save(words, routed ? "cas-global.spv" : "cas-storage.spv");
    }
}

TEST_CASE("Ampere U64 surface max preserves the register pair and descriptor", "[ampere][u64]") {
    for (const bool bindless : {false, true}) {
        for (const bool returns_value : {false, true}) {
            auto native = LoggedMax;
            native.words[0] = (native.words[0] & ~0xfffU) |
                (bindless ? (returns_value ? 0x394U : 0x3a0U) : (returns_value ? 0x593U : 0x59fU));
            native.words[0] |= 8U << 16;
            native.words[2] = (returns_value ? 0x011ee400U : 0x0110e400U) | (bindless ? 12U : 0U);
            if (bindless) native.words[1] &= 0xe00000ffU;
            for (u32 guard = 0; guard < 15; ++guard) {
                native.words[0] = (native.words[0] & ~0xf000U) | (guard << 12);
                const auto lowered = LowerToMaxwell(std::array{native, Exit});
                REQUIRE(static_cast<bool>(lowered));
                const u64 word = lowered.instructions.front();
                CHECK(((word >> 16) & 15) == guard);
                CHECK(((word >> 8) & 255) == 16);
                CHECK(((word >> (returns_value ? 51 : 20)) & 7) == 2);
                CHECK(((word >> (returns_value ? 29 : 24)) & 7) == 2);
                CHECK(((word >> 33) & 7) == 3);
                CHECK(((word >> (returns_value ? 20 : 0)) & 255) == 2);
                CHECK(lowered.required_texture_buffer == (bindless ? ~0U : 3U));
                if (bindless) CHECK(((word >> 39) & 255) == 12);
            }
        }
    }
}

TEST_CASE("Ampere U64 surface max rejects unsupported operations and malformed pairs", "[ampere][u64]") {
    for (u32 op = 0; op < 8; ++op) {
        auto native = LoggedMax;
        native.words[2] = (native.words[2] & ~0x03800000U) | (op << 23);
        CHECK(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})) == (op == 2));
    }
    for (const u32 data : {1U, 253U, 254U, 255U}) {
        auto native = LoggedMax;
        native.words[1] = (native.words[1] & ~255U) | data;
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{native, Exit})));
    }
    auto buffer = LoggedMax;
    buffer.words[1] = (buffer.words[1] & ~0xe0000000U) | 0x20000000U;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{buffer, Exit})));
    auto signed64 = LoggedMax;
    signed64.words[2] |= 0x200;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{signed64, Exit})));
    auto span = LoggedMax;
    span.words[0] = (span.words[0] & ~0xff000000U) | (242U << 24);
    span.words[1] = (span.words[1] & ~0xe0000000U) | (5U << 29); // Three coordinates cross R244.
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{span, Exit})));
    for (u32 destination : {1U, 253U, 254U, 255U}) {
        auto returning = LoggedMax;
        returning.words[0] = (returning.words[0] & ~0x00ff0fffU) | (destination << 16) | 0x593;
        returning.words[2] = 0x011ee400;
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{returning, Exit})));
    }
}

TEST_CASE("U64 SUATOM reads both operand words and returns both old-value words", "[ampere][u64]") {
    for (bool bindless : {false, true}) {
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        AtomicEnvironment env{{}};
        Shader::Maxwell::TranslatorVisitor visitor{env, block};
        const u64 word = 0xea00000000070000ULL | 8U | (16ULL << 8) | (2ULL << 20) |
            (2ULL << 29) | (3ULL << 33) | (2ULL << 51) |
            (bindless ? (1ULL << 54) | (12ULL << 39) : 0);
        visitor.SUATOM(word);
        IR::Inst* atomic = nullptr;
        std::array<bool, 2> read{}, written{};
        for (auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == (bindless ? IR::Opcode::BindlessImageAtomicUMax64 :
                                               IR::Opcode::BoundImageAtomicUMax64)) atomic = &inst;
            if (inst.GetOpcode() == IR::Opcode::GetRegister) {
                read[0] |= inst.Arg(0).Reg() == IR::Reg::R2;
                read[1] |= inst.Arg(0).Reg() == IR::Reg::R3;
            }
            if (inst.GetOpcode() == IR::Opcode::SetRegister) {
                written[0] |= inst.Arg(0).Reg() == IR::Reg::R8;
                written[1] |= inst.Arg(0).Reg() == IR::Reg::R9;
            }
        }
        REQUIRE(atomic != nullptr);
        CHECK(atomic->Type() == IR::Type::U64);
        CHECK(atomic->Arg(2).Type() == IR::Type::U64);
        CHECK(atomic->MayHaveSideEffects());
        CHECK(read == std::array{true, true});
        CHECK(written == std::array{true, true});
    }
}

TEST_CASE("Ampere U64 surface max shadows both source words", "[ampere][u64]") {
    auto native = LoggedMax;
    native.words[1] = (native.words[1] & ~255U) | 246U;
    const auto lowered = LowerToMaxwell(std::array{native, Exit});
    REQUIRE(static_cast<bool>(lowered));
    bool copied_low = false, copied_high = false;
    for (u64 word : lowered.instructions) {
        if ((word & 0xfff8000000000000ULL) != 0x5c98000000000000ULL) continue;
        if (((word >> 44) & 15) != 10 || ((word >> 10) & 3) != 1) continue;
        copied_low |= ((word >> 20) & 255) == 246;
        copied_high |= ((word >> 20) & 255) == 247;
    }
    CHECK(copied_low);
    CHECK(copied_high);
}

TEST_CASE("Logged U64 max reaches a single 64-bit image atomic in SPIRV", "[ampere][u64][spirv]") {
    const std::array<u64, 6> values{0x00000000ffffffffULL, 0x0000000100000000ULL,
                                   0x7fffffffffffffffULL, 0x8000000000000000ULL,
                                   0xffffffffffffffffULL, 0};
    for (u32 i = 0; i < values.size(); ++i) {
        const u64 value = values[i];
        auto native = LoggedMax;
        native.words[0] = (native.words[0] & ~0xf000U) | 0x7000U; // PT, fixture is normally @P5.
        // Last module: low = 0xffffffff, high = local invocation X, all lanes contend on (0,0).
        const Instruction high = i == 5 ? Instruction{{0x00037919, 0, 33U << 8, 0}} :
            Move(3, static_cast<u32>(value >> 32));
        const auto lowered = LowerToMaxwell(std::array{Move(16, 0), Move(17, 0),
            Move(2, i == 5 ? 0xffffffffU : static_cast<u32>(value)), high, native, Exit});
        REQUIRE(static_cast<bool>(lowered));
        AtomicEnvironment env{lowered.instructions};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<IR::Inst> inst_pool;
        Shader::ObjectPool<IR::Block> block_pool;
        Shader::HostTranslateInfo host;
        host.support_int64 = true;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
        REQUIRE(program.info.uses_atomic_image_u64);
        REQUIRE(program.info.uses_int64);
        REQUIRE(program.info.image_descriptors.size() == 1);
        CHECK(program.info.image_descriptors.front().format == Shader::ImageFormat::R64_UINT);
        CHECK(program.info.image_descriptors.front().is_written);
        CHECK(program.info.image_descriptors.front().is_integer);
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.support_int64 = true;
        CHECK_THROWS(Shader::Backend::SPIRV::EmitSPIRV(profile, program));
        profile.support_image_int64_atomics = true;
        profile.support_int64 = false;
        CHECK_THROWS(Shader::Backend::SPIRV::EmitSPIRV(profile, program));
        profile.support_int64 = true;
        const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        u32 atomics = 0, image_type = 0, integer64_type = 0;
        bool image64_cap = false, atomic64_cap = false;
        for (size_t offset = 5; offset < words.size();) {
            const u32 size = words[offset] >> 16;
            REQUIRE(size > 0);
            REQUIRE(offset + size <= words.size());
            const auto op = static_cast<spv::Op>(words[offset] & 0xffff);
            if (op == spv::Op::OpTypeInt && words[offset + 2] == 64) integer64_type = words[offset + 1];
            if (op == spv::Op::OpTypeImage) {
                image_type = words[offset + 2];
                CHECK(words[offset + 8] == static_cast<u32>(spv::ImageFormat::R64ui));
            }
            if (op == spv::Op::OpCapability) {
                image64_cap |= words[offset + 1] == static_cast<u32>(spv::Capability::Int64ImageEXT);
                atomic64_cap |= words[offset + 1] == static_cast<u32>(spv::Capability::Int64Atomics);
            }
            if (op == spv::Op::OpAtomicUMax) {
                ++atomics;
                CHECK(words[offset + 1] == integer64_type);
            }
            offset += size;
        }
        CHECK(atomics == 1);
        REQUIRE(integer64_type != 0);
        CHECK(image_type == integer64_type);
        CHECK(image64_cap);
        CHECK(atomic64_cap);
        Save(words, "u64-max-" + std::to_string(i) + ".spv");
    }
}
