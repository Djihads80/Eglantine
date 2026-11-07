// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

std::vector<u32> EmitAtomicHelpers(bool native_half, bool explicit_shared) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {256, 1, 1};
    program.shared_memory_size = 16;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    auto& info = program.info;
    info.storage_buffers_descriptors = {{0, 0, 1, true}};
    info.used_storage_buffer_types = IR::Type::U32;
    info.uses_shared_increment = true;
    info.uses_shared_decrement = true;
    info.uses_global_increment = true;
    info.uses_global_decrement = true;
    info.uses_atomic_f32_add = true;
    info.uses_fp16 = native_half;
    info.uses_atomic_f16x2_add = native_half;
    info.uses_atomic_f16x2_min = native_half;
    info.uses_atomic_f16x2_max = native_half;
    info.uses_atomic_f32x2_add = !native_half;
    info.uses_atomic_f32x2_min = !native_half;
    info.uses_atomic_f32x2_max = !native_half;
    info.uses_int8 = true;
    info.uses_int16 = true;
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.support_explicit_workgroup_layout = explicit_shared;
    profile.support_workgroup_layout_8bit_access = explicit_shared;
    profile.support_workgroup_layout_16bit_access = explicit_shared;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* directory = std::getenv("EDEN_ATOMIC_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const auto name = std::string{native_half ? "native-half" : "packed-half"} +
                          (explicit_shared ? "-explicit" : "-implicit") + ".spv";
        std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size() * sizeof(u32)));
        REQUIRE(file.good());
    }
    return code;
}
}

TEST_CASE("SPIR-V CAS retries use the exchange result as the next expected word",
          "[video_core][spirv][atomic_retry]") {
    for (const bool native_half : {true, false}) {
        for (const bool explicit_shared : {false, true}) {
            CAPTURE(native_half, explicit_shared);
            const auto code = EmitAtomicHelpers(native_half, explicit_shared);
            const auto exchanges = Instructions(code, spv::Op::OpAtomicCompareExchange);
            const auto loads = Instructions(code, spv::Op::OpLoad);
            const auto stores = Instructions(code, spv::Op::OpStore);
            const auto variables = Instructions(code, spv::Op::OpVariable);
            const auto integers = Instructions(code, spv::Op::OpTypeInt);
            const auto comparisons = Instructions(code, spv::Op::OpIEqual);
            REQUIRE(exchanges.size() == (explicit_shared ? 9U : 11U));
            for (const auto& exchange : exchanges) {
                REQUIRE(exchange.size() == 8);
                const u32 type = exchange[0];
                const u32 result = exchange[1];
                const u32 memory = exchange[2];
                const u32 expected = exchange[7];
                CHECK(std::ranges::find(integers, std::vector<u32>{type, 32, 0}) != integers.end());
                const auto expected_load = std::ranges::find_if(
                    loads, [=](const auto& load) { return load[1] == expected; });
                REQUIRE(expected_load != loads.end());
                CHECK((*expected_load)[0] == type);
                const u32 retry = (*expected_load)[2];
                CHECK(retry != memory);
                const auto variable = std::ranges::find_if(
                    variables, [=](const auto& var) { return var[1] == retry; });
                REQUIRE(variable != variables.end());
                CHECK((*variable)[2] == static_cast<u32>(spv::StorageClass::Function));
                REQUIRE(std::ranges::count_if(stores, [=](const auto& store) {
                    return store[0] == retry;
                }) == 2);
                CHECK(std::ranges::find(stores, std::vector<u32>{retry, result}) != stores.end());
                const auto initial = std::ranges::find_if(
                    loads, [=](const auto& load) { return load[2] == memory; });
                REQUIRE(initial != loads.end());
                CHECK((*initial)[0] == type);
                CHECK(std::ranges::count_if(loads, [=](const auto& load) {
                    return load[2] == memory;
                }) == 1);
                CHECK(std::ranges::find(stores, std::vector<u32>{retry, (*initial)[1]}) != stores.end());
                CHECK(std::ranges::any_of(comparisons, [=](const auto& comparison) {
                    return (comparison[2] == result && comparison[3] == expected) ||
                           (comparison[3] == result && comparison[2] == expected);
                }));
            }
        }
    }
}
