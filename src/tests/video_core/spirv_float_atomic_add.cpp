// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace {
namespace IR = Shader::IR;

constexpr u32 OpCapability = 17;
constexpr u32 OpAtomicIAdd = 234;
constexpr u32 OpAtomicFAddEXT = 6035;
constexpr u32 CapabilityAtomicFloat32AddEXT = 6033;

size_t Count(const std::vector<u32>& code, u32 opcode, u32 first_operand = ~0U) {
    size_t count = 0;
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count = code[offset] >> 16;
        REQUIRE(word_count > 0);
        REQUIRE(offset + word_count <= code.size());
        if ((code[offset] & 0xffff) == opcode &&
            (first_operand == ~0U || (word_count > 1 && code[offset + 1] == first_operand))) {
            ++count;
        }
        offset += word_count;
    }
    return count;
}

std::vector<u32> EmitAtomicAdd(bool native) {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
    program.info.used_storage_buffer_types = IR::Type::U32 | IR::Type::F32;
    program.info.uses_atomic_f32_add = true;
    block.AppendNewInst(IR::Opcode::StorageAtomicAddF32,
                        {IR::Value{0U}, IR::Value{16U}, IR::Value{1.5f}});
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_descriptor_aliasing = true;
    profile.support_float32_atomic_add = native;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}
} // Anonymous namespace

TEST_CASE("SPIR-V float atomic add is native where the device has it",
          "[video_core][spirv][float_atomic_add]") {
    const std::vector<u32> code = EmitAtomicAdd(true);
    CHECK(Count(code, OpAtomicFAddEXT) == 1);
    CHECK(Count(code, OpAtomicIAdd) == 0);
    CHECK(Count(code, OpCapability, CapabilityAtomicFloat32AddEXT) == 1);
}

TEST_CASE("SPIR-V float atomic add keeps the compare-exchange loop otherwise",
          "[video_core][spirv][float_atomic_add]") {
    const std::vector<u32> code = EmitAtomicAdd(false);
    CHECK(Count(code, OpAtomicFAddEXT) == 0);
    CHECK(Count(code, OpCapability, CapabilityAtomicFloat32AddEXT) == 0);
}
