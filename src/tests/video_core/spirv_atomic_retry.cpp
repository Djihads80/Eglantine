// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"

namespace {
std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count != 0);
        REQUIRE(offset + count <= code.size());
        if ((code[offset] & 0xffffU) == opcode) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + count);
        }
        offset += count;
    }
    return result;
}
} // namespace

TEST_CASE("Storage float atomic retries use the returned word", "[shader_recompiler][atomic_retry]") {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    block.AppendNewInst(IR::Opcode::StorageAtomicAddF32, {ir.Imm32(0), ir.Imm32(0), ir.Imm32(1.0f)});

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_atomic_f32_add = true;
    program.info.used_storage_buffer_types = IR::Type::U32;
    program.info.storage_buffers_descriptors = {{0, 0, 1, true}};

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);

    constexpr u32 OpLoad = 61;
    constexpr u32 OpStore = 62;
    constexpr u32 OpAtomicCompareExchange = 230;
    const auto exchanges = Instructions(code, OpAtomicCompareExchange);
    REQUIRE(exchanges.size() == 1);
    const auto& exchange = exchanges.front();
    REQUIRE(exchange.size() == 8);

    const auto loads = Instructions(code, OpLoad);
    const auto expected = std::ranges::find_if(loads, [&](const auto& load) {
        return load[1] == exchange[7];
    });
    REQUIRE(expected != loads.end());
    const auto stores = Instructions(code, OpStore);
    CHECK(std::ranges::any_of(stores, [&](const auto& store) {
        return store[0] == (*expected)[2] && store[1] == exchange[1];
    }));
}
