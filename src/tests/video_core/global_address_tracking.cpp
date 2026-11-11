// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
class StubEnvironment final : public Shader::Environment {
public:
    StubEnvironment() { stage = Shader::Stage::Compute; }
    bool native{};
    bool IsNativeProgram() const override { return native; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};
} // namespace

TEST_CASE("Global address tracking stops at a memory load", "[shader_recompiler][global_memory]") {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto low = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x110));
    const auto high = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x114));
    const auto source = ir.PackUint2x32(ir.CompositeConstruct(low, high));
    const auto loaded = ir.LoadGlobal64(source);
    const auto destination = ir.PackUint2x32(ir.CompositeConstruct(
        ir.CompositeExtract(loaded, 0), ir.CompositeExtract(loaded, 1)));
    ir.WriteGlobal32(destination, ir.Imm32(1));

    IR::Program program;
    program.post_order_blocks.push_back(&block);
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    StubEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    const auto has = [&](IR::Opcode opcode) {
        return std::ranges::any_of(block.Instructions(), [=](const IR::Inst& inst) {
            return inst.GetOpcode() == opcode;
        });
    };
    REQUIRE(has(IR::Opcode::LoadStorage64));
    REQUIRE(has(IR::Opcode::WriteGlobal32));
    REQUIRE_FALSE(has(IR::Opcode::WriteStorage32));
}

TEST_CASE("Pointer table entries are charged only for bindings the first pass left free",
          "[shader_recompiler][global_memory]") {
    using namespace Shader;
    ObjectPool<IR::Inst> inst_pool;
    IR::Block entry_block{inst_pool};
    IR::Block loop_block{inst_pool};
    entry_block.AddBranch(&loop_block);
    loop_block.AddBranch(&loop_block);

    IR::Program program;
    program.stage = Stage::Compute;
    program.blocks = {&entry_block, &loop_block};
    program.post_order_blocks = {&loop_block, &entry_block};

    const auto word = [](IR::IREmitter& ir, u32 offset) {
        return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
    };
    const auto address = [](IR::IREmitter& ir, const IR::U32& low, const IR::U32& high) {
        return ir.PackUint2x32(ir.CompositeConstruct(low, high));
    };

    IR::IREmitter entry{entry_block};
    const IR::U32 lane{entry.LocalInvocationIdX()};
    const IR::U1 first{entry.ILessThan(lane, entry.Imm32(16), false)};
    const IR::U32 holder_low{entry.Select(first, word(entry, 0x30), word(entry, 0x38))};
    const IR::U32 holder_high{entry.Select(first, word(entry, 0x34), word(entry, 0x3c))};
    const IR::Value table{entry.LoadGlobal64(address(entry, holder_low, holder_high))};
    for (u32 slot = 0; slot < Info::MAX_SSBOS - 4; ++slot) {
        const u32 offset{0x100 + slot * 8};
        static_cast<void>(entry.LoadGlobal32(address(entry, word(entry, offset), word(entry, offset + 4))));
    }
    IR::IREmitter body{loop_block};
    const IR::U32 record{body.IAdd(IR::U32{body.CompositeExtract(table, 0)},
                                   body.ShiftLeftLogical(lane, body.Imm32(4)))};
    static_cast<void>(body.LoadGlobal128(address(body, record, IR::U32{body.CompositeExtract(table, 1)})));

    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    StubEnvironment env;
    env.native = true;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    CHECK(program.info.storage_buffers_descriptors.size() == Info::MAX_SSBOS);
    CHECK(std::ranges::count_if(program.info.storage_buffers_descriptors,
                                [](const auto& desc) { return desc.indirect_offset != ~0U; }) == 2);
}

TEST_CASE("A stage binds up to 64 storage buffers when the device allows them",
          "[shader_recompiler][global_memory]") {
    using namespace Shader;
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::IREmitter ir{block};
    for (u32 slot = 0; slot < 40; ++slot) {
        const u32 offset{0x100 + slot * 8};
        const IR::U32 low{ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset))};
        const IR::U32 high{ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset + 4))};
        static_cast<void>(ir.LoadGlobal32(ir.PackUint2x32(ir.CompositeConstruct(low, high))));
    }

    IR::Program program;
    program.stage = Stage::Compute;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    StubEnvironment env;
    env.native = true;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    CHECK(program.info.storage_buffers_descriptors.size() == 40);
}

TEST_CASE("Dual vertex halves that bind more storage buffers than the stage are rejected",
          "[shader_recompiler][global_memory]") {
    using namespace Shader;
    Info base;
    base.storage_buffers_descriptors.push_back({.cbuf_index = 0, .cbuf_offset = 0x110, .count = 1});
    Info source;
    source.storage_buffers_descriptors.push_back({.cbuf_index = 0, .cbuf_offset = 0x110, .count = 1});
    source.storage_buffers_descriptors.push_back({.cbuf_index = 0, .cbuf_offset = 0x120, .count = 1});
    Optimization::JoinStorageInfo(base, source, 2);
    CHECK(base.storage_buffers_descriptors.size() == 2);
    source.storage_buffers_descriptors.push_back({.cbuf_index = 0, .cbuf_offset = 0x130, .count = 1});
    CHECK_THROWS_AS(Optimization::JoinStorageInfo(base, source, 2), NotImplementedException);
}
