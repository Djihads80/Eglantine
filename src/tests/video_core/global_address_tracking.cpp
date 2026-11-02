// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

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
    Optimization::GlobalMemoryToStorageBufferPass(program, host);

    const auto has = [&](IR::Opcode opcode) {
        return std::ranges::any_of(block.Instructions(), [=](const IR::Inst& inst) {
            return inst.GetOpcode() == opcode;
        });
    };
    REQUIRE(has(IR::Opcode::LoadStorage64));
    REQUIRE(has(IR::Opcode::WriteGlobal32));
    REQUIRE_FALSE(has(IR::Opcode::WriteStorage32));
}
