// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, spv::Op opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == static_cast<u32>(opcode)) {
            const auto words = std::span{code}.subspan(offset + 1, size - 1);
            result.emplace_back(words.begin(), words.end());
        }
        offset += size;
    }
    return result;
}

struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile{};

    Fixture(bool aliasing, u32 alignment) {
        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010300;
        profile.support_int64 = true;
        profile.unified_descriptor_binding = true;
        profile.support_descriptor_aliasing = aliasing;
        profile.min_ssbo_alignment = alignment;
        auto& info = program.info;
        info.constant_buffer_descriptors = {{0, 1}};
        info.constant_buffer_mask = 1;
        info.constant_buffer_used_sizes[0] = 0x400;
        info.used_constant_buffer_types = IR::Type::U32 | IR::Type::U32x2;
        info.used_storage_buffer_types = IR::Type::U32 | IR::Type::U32x2 | IR::Type::U32x4;
    }

    std::vector<u32> Emit(const std::string& name) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EGLANTINE_STORAGE_ALIGNMENT_TEST_OUTPUT")) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (name + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

// Access chains whose result points into a storage buffer vector of 'components' words.
size_t VectorAccessChains(const std::vector<u32>& code, u32 components) {
    std::set<u32> uints;
    for (const auto& op : Instructions(code, spv::Op::OpTypeInt)) {
        if (op[1] == 32 && op[2] == 0) {
            uints.insert(op[0]);
        }
    }
    std::set<u32> vectors;
    for (const auto& op : Instructions(code, spv::Op::OpTypeVector)) {
        if (uints.contains(op[1]) && op[2] == components) {
            vectors.insert(op[0]);
        }
    }
    std::set<u32> pointers;
    for (const auto& op : Instructions(code, spv::Op::OpTypePointer)) {
        if (op[1] == static_cast<u32>(spv::StorageClass::StorageBuffer) &&
            vectors.contains(op[2])) {
            pointers.insert(op[0]);
        }
    }
    return static_cast<size_t>(std::ranges::count_if(
        Instructions(code, spv::Op::OpAccessChain),
        [&](const auto& op) { return pointers.contains(op[0]); }));
}

IR::Value CbufVector(IR::IREmitter& ir, u32 words) {
    const auto word = [&](u32 index) {
        return ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x80 + index * 4));
    };
    return words == 2 ? ir.CompositeConstruct(word(0), word(1))
                      : ir.CompositeConstruct(word(0), word(1), word(2), word(3));
}
} // namespace

TEST_CASE("Storage vector accesses use the vector view only where its index is exact",
          "[video_core][spirv][storage_alignment]") {
    for (const bool aliasing : {true, false}) {
        for (const u32 alignment : {4U, 8U, 16U}) {
            for (const u32 bytes : {8U, 16U}) {
                for (const bool write : {false, true}) {
                    for (const std::optional<u32> immediate :
                         {std::optional<u32>{}, std::optional<u32>{0}, std::optional<u32>{4},
                          std::optional<u32>{8}, std::optional<u32>{12},
                          std::optional<u32>{16}}) {
                        CAPTURE(aliasing, alignment, bytes, write, immediate.value_or(~0U));
                        Fixture fixture{aliasing, alignment};
                        fixture.program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
                        IR::IREmitter ir{fixture.block};
                        const IR::U32 offset{immediate
                                                 ? ir.Imm32(*immediate)
                                                 : ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x40))};
                        const u32 words = bytes / 4;
                        const IR::Opcode opcode =
                            write ? (bytes == 8 ? IR::Opcode::WriteStorage64
                                                : IR::Opcode::WriteStorage128)
                                  : (bytes == 8 ? IR::Opcode::LoadStorage64
                                                : IR::Opcode::LoadStorage128);
                        if (write) {
                            fixture.block.AppendNewInst(
                                opcode, {ir.Imm32(0), offset, CbufVector(ir, words)});
                        } else {
                            const IR::Value value{&*fixture.block.PrependNewInst(
                                fixture.block.end(), opcode, {ir.Imm32(0), offset})};
                            fixture.block.AppendNewInst(
                                IR::Opcode::WriteStorage32,
                                {ir.Imm32(0), ir.Imm32(0x100),
                                 ir.CompositeExtract(value, words - 1)});
                        }
                        const auto code = fixture.Emit(
                            std::string{write ? "store" : "load"} + std::to_string(bytes * 8) +
                            "-align" + std::to_string(alignment) +
                            (aliasing ? "-alias" : "-noalias") +
                            (immediate ? "-imm" + std::to_string(*immediate) : "-dynamic"));
                        const bool exact = aliasing && (immediate ? *immediate % bytes == 0
                                                                  : alignment >= bytes);
                        CHECK(VectorAccessChains(code, words) == (exact ? 1U : 0U));
                    }
                }
            }
        }
    }
}

TEST_CASE("Global vector accesses read words from the buffer the address falls in",
          "[video_core][spirv][storage_alignment]") {
    // The NVN storage descriptor path needs descriptor aliasing, as before.
    for (const bool aliasing : {true}) {
        for (const u32 alignment : {4U, 8U, 16U}) {
            CAPTURE(aliasing, alignment);
            Fixture fixture{aliasing, alignment};
            auto& info = fixture.program.info;
            info.uses_global_memory = true;
            info.uses_int64 = true;
            info.nvn_buffer_base = 0x310;
            info.nvn_buffer_used.set(0);
            info.nvn_buffer_used.set(1);
            info.storage_buffers_descriptors = {{0, 0x310, 1, true}, {0, 0x320, 1, true}};
            IR::IREmitter ir{fixture.block};
            const auto address = [&](u32 offset) {
                return ir.PackUint2x32(
                    ir.CompositeConstruct(ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)),
                                          ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset + 4))));
            };
            const IR::Value vector2{ir.LoadGlobal64(address(0x40))};
            const IR::Value vector4{ir.LoadGlobal128(address(0x48))};
            ir.WriteGlobal64(address(0x50), vector2);
            ir.WriteGlobal128(address(0x58), vector4);
            const auto code = fixture.Emit("global-align" + std::to_string(alignment) +
                                           (aliasing ? "-alias" : "-noalias"));
            // Each vector function looks in both buffers.
            CHECK(VectorAccessChains(code, 2) == (aliasing && alignment >= 8 ? 4U : 0U));
            CHECK(VectorAccessChains(code, 4) == (aliasing && alignment >= 16 ? 4U : 0U));
            // The split functions search the buffers themselves.
            const size_t word_calls = 0;
            CHECK(Instructions(code, spv::Op::OpFunctionCall).size() == 4 + word_calls);
        }
    }
}
