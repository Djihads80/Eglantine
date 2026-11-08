// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

struct ShuffleModule {
    std::vector<u32> code;
    std::array<u32, 3> results;
};

ShuffleModule EmitShuffles(Stage stage, u32 subgroup_size, bool fragment_fallback,
                           bool general_fallback = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.workgroup_size = {64, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_subgroup_invocation_id = true;
    program.info.uses_subgroup_shuffles = true;
    IR::IREmitter ir{block};
    const auto lane = ir.LaneId();
    const auto value = ir.IAdd(lane, ir.Imm32(100));
    const std::array<IR::U32, 3> results{
        ir.ShuffleIndex(value, ir.BitwiseXor(lane, ir.Imm32(1)), ir.Imm32(31), ir.Imm32(0)),
        ir.ShuffleUp(value, ir.Imm32(1), ir.Imm32(0), ir.Imm32(0)),
        ir.ShuffleDown(value, ir.Imm32(1), ir.Imm32(31), ir.Imm32(0)),
    };
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.support_vote = true;
    profile.support_shuffle_relative = true;
    profile.max_subgroup_size = subgroup_size;
    profile.warp_size_potentially_larger_than_guest = subgroup_size > 32;
    profile.has_broken_spirv_fragment_shuffle = fragment_fallback;
    profile.has_broken_spirv_subgroup_shuffle = general_fallback;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* directory = std::getenv("EDEN_SHUFFLE_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const auto name = std::to_string(static_cast<u32>(stage)) + "-" +
                          std::to_string(subgroup_size) + "-" +
                          std::to_string(fragment_fallback) + "-" +
                          std::to_string(general_fallback) + ".spv";
        std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size() * sizeof(u32)));
        REQUIRE(file.good());
    }
    return {std::move(code), {results[0].Inst()->Definition<u32>(),
                              results[1].Inst()->Definition<u32>(),
                              results[2].Inst()->Definition<u32>()}};
}
}

TEST_CASE("Fragment shuffle fallback preserves host subgroup partitions",
          "[video_core][spirv][fragment_shuffle]") {
    for (const u32 subgroup_size : {32U, 64U}) {
        const auto module = EmitShuffles(Stage::Fragment, subgroup_size, true);
        std::map<u32, std::vector<u32>> definitions;
        std::vector<u32> broadcast_lanes;
        u32 invocation_variable{};
        for (size_t offset = 5; offset < module.code.size();) {
            const u32 size = module.code[offset] >> 16;
            REQUIRE(size != 0);
            REQUIRE(offset + size <= module.code.size());
            const auto words = std::span{module.code}.subspan(offset, size);
            const auto op = static_cast<spv::Op>(words[0] & 0xffffU);
            CHECK(op != spv::Op::OpGroupNonUniformShuffle);
            CHECK(op != spv::Op::OpGroupNonUniformShuffleUp);
            CHECK(op != spv::Op::OpGroupNonUniformShuffleDown);
            bool has_result{}, has_type{};
            spv::HasResultAndType(op, &has_result, &has_type);
            if (has_result && has_type) {
                definitions.emplace(words[2], std::vector<u32>{words.begin(), words.end()});
            }
            if (op == spv::Op::OpGroupNonUniformBroadcast) {
                broadcast_lanes.push_back(words[5]);
            }
            if (op == spv::Op::OpDecorate &&
                words[2] == static_cast<u32>(spv::Decoration::BuiltIn) &&
                words[3] == static_cast<u32>(spv::BuiltIn::SubgroupLocalInvocationId)) {
                invocation_variable = words[1];
            }
            offset += size;
        }
        REQUIRE(broadcast_lanes.size() == 3 * subgroup_size);
        REQUIRE(invocation_variable != 0);
        for (u32 index = 0; index < broadcast_lanes.size(); ++index) {
            CHECK(definitions.at(broadcast_lanes[index])[3] == index % subgroup_size);
        }
        const auto evaluate = [&](auto&& self, u32 id, u32 lane) -> u32 {
            const auto& words = definitions.at(id);
            const auto arg = [&](size_t index) { return self(self, words[index], lane); };
            switch (static_cast<spv::Op>(words[0] & 0xffffU)) {
            case spv::Op::OpConstant: return words[3];
            case spv::Op::OpLoad:
                REQUIRE(words[3] == invocation_variable);
                return lane;
            case spv::Op::OpIAdd: return arg(3) + arg(4);
            case spv::Op::OpISub: return arg(3) - arg(4);
            case spv::Op::OpBitwiseAnd: return arg(3) & arg(4);
            case spv::Op::OpBitwiseOr: return arg(3) | arg(4);
            case spv::Op::OpBitwiseXor: return arg(3) ^ arg(4);
            case spv::Op::OpNot: return ~arg(3);
            case spv::Op::OpShiftLeftLogical: return arg(3) << arg(4);
            case spv::Op::OpShiftRightLogical: return arg(3) >> arg(4);
            case spv::Op::OpIEqual: return arg(3) == arg(4);
            case spv::Op::OpSLessThanEqual:
                return std::bit_cast<s32>(arg(3)) <= std::bit_cast<s32>(arg(4));
            case spv::Op::OpSGreaterThanEqual:
                return std::bit_cast<s32>(arg(3)) >= std::bit_cast<s32>(arg(4));
            case spv::Op::OpSelect: return arg(3) ? arg(4) : arg(5);
            case spv::Op::OpGroupNonUniformBroadcast:
                CHECK(arg(5) / 32 == lane / 32);
                return self(self, words[4], arg(5));
            default: FAIL("Unexpected shuffle opcode " << (words[0] & 0xffffU));
            }
            return 0;
        };
        for (u32 lane = 0; lane < subgroup_size; ++lane) {
            CAPTURE(subgroup_size, lane);
            const u32 guest_lane = lane % 32;
            CHECK(evaluate(evaluate, module.results[0], lane) == 100 + (guest_lane ^ 1));
            CHECK(evaluate(evaluate, module.results[1], lane) ==
                  100 + (guest_lane == 0 ? 0 : guest_lane - 1));
            CHECK(evaluate(evaluate, module.results[2], lane) ==
                  100 + (guest_lane == 31 ? 31 : guest_lane + 1));
        }
    }
}

TEST_CASE("Fragment workaround leaves other stages on native shuffles",
          "[video_core][spirv][fragment_shuffle]") {
    for (const auto stage : {Stage::Fragment, Stage::Compute, Stage::VertexB}) {
        for (const bool fragment_fallback : {false, true}) {
            for (const bool general_fallback : {false, true}) {
                const auto module = EmitShuffles(stage, 64, fragment_fallback, general_fallback);
                size_t broadcasts{}, shuffles{};
                for (size_t offset = 5; offset < module.code.size();) {
                    const u32 size = module.code[offset] >> 16;
                    REQUIRE(size != 0);
                    const auto op = static_cast<spv::Op>(module.code[offset] & 0xffffU);
                    broadcasts += op == spv::Op::OpGroupNonUniformBroadcast;
                    shuffles += op == spv::Op::OpGroupNonUniformShuffle ||
                                op == spv::Op::OpGroupNonUniformShuffleUp ||
                                op == spv::Op::OpGroupNonUniformShuffleDown;
                    offset += size;
                }
                const bool fallback = general_fallback ||
                                      (fragment_fallback && stage == Stage::Fragment);
                CHECK(broadcasts == (fallback ? 192 : 0));
                CHECK(shuffles == (fallback ? 0 : 3));
            }
        }
    }
}
