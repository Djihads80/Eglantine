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

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using namespace Shader;

struct TensorProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit TensorProgram(u32 count = 1, u32 software_depth = 0) {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.uses_subgroup_shuffles = count != 0 && software_depth != 0;
        program.info.uses_subgroup_invocation_id = count != 0;
        profile.supported_spirv = 0x00010500;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
        profile.max_subgroup_size = 32;
        IR::IREmitter ir{block};
        if (count == 0) {
            return;
        }
        const auto lane = ir.LaneId();
        std::array<IR::U32, 8> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            inputs[i] = software_depth ? ir.IAdd(ir.Imm32(0x3c003c00U + i * 0x01000100U),
                                                 ir.ShiftLeftLogical(lane, ir.Imm32(3)))
                                       : ir.IAdd(ir.Imm32((i + 1) * 0x01000100U), lane);
        }
        for (u32 i = 0; i < count; ++i) {
            const auto result =
                ir.TensorMulAdd(ir.CompositeConstruct(inputs[0], inputs[1], inputs[2], inputs[3]),
                                ir.CompositeConstruct(inputs[4], inputs[5]),
                                ir.CompositeConstruct(inputs[6], inputs[7]), software_depth);
            inputs[6] = IR::U32{ir.CompositeExtract(result, 0)};
            inputs[7] = IR::U32{ir.CompositeExtract(result, 1)};
        }
        if (software_depth) {
            program.info.used_storage_buffer_types = IR::Type::U32;
            program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
            program.info.uses_local_invocation_id = true;
            const auto offset = ir.ShiftLeftLogical(ir.LocalInvocationIdX(), ir.Imm32(3));
            block.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), offset, inputs[6]});
            block.AppendNewInst(IR::Opcode::WriteStorage32,
                                {ir.Imm32(0), ir.IAdd(offset, ir.Imm32(4)), inputs[7]});
        }
    }

    std::vector<u32> Emit(std::string_view name = {}) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EDEN_TENSOR_TEST_OUTPUT");
            directory && !name.empty()) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == opcode) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + size);
        }
        offset += size;
    }
    return result;
}

bool HasCapability(const std::vector<u32>& code, u32 capability) {
    const auto capabilities = Instructions(code, 17);
    return std::ranges::any_of(capabilities,
                               [=](const auto& words) { return words[0] == capability; });
}

} // namespace

TEST_CASE("Software HMMA shares a bounded loop without tensor extensions",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram single{1, depth};
        const auto first = single.Emit(depth == 8 ? "software-k8" : "software-k16");
        REQUIRE_FALSE(HasCapability(first, 5357));
        REQUIRE(Instructions(first, 246).size() == 1); // OpLoopMerge
        REQUIRE(Instructions(first, 57).size() == 1);  // OpFunctionCall
        TensorProgram repeated{64, depth};
        const auto many = repeated.Emit(depth == 8 ? "software-k8-many" : "software-k16-many");
        REQUIRE(Instructions(many, 246).size() == 1);
        REQUIRE(Instructions(many, 57).size() == 64);
        // Three fragment-vector constructions, the call and two result extracts
        // take at most 40 words per site; the matrix loop itself is never duplicated.
        REQUIRE(many.size() < first.size() + 64 * 40);
    }
}

TEST_CASE("Software HMMA rounds only on uniform K8 boundaries", "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        const auto code = fixture.Emit();
        std::map<u32, u32> constants;
        for (const auto& words : Instructions(code, 43)) { // OpConstant
            constants.emplace(words[1], words[2]);
        }

        const auto selections = Instructions(code, 247); // OpSelectionMerge
        REQUIRE(selections.size() == 1);
        REQUIRE(selections.front()[1] == 2); // DontFlatten
        const u32 merge = selections.front()[0];
        const auto branches = Instructions(code, 250); // OpBranchConditional
        const auto branch =
            std::ranges::find_if(branches, [=](const auto& words) { return words[2] == merge; });
        REQUIRE(branch != branches.end());
        const u32 condition = (*branch)[0];
        const u32 round_block = (*branch)[1];
        REQUIRE(round_block != merge);

        const auto comparisons = Instructions(code, 170); // OpIEqual
        const auto comparison = std::ranges::find_if(
            comparisons, [=](const auto& words) { return words[1] == condition; });
        REQUIRE(comparison != comparisons.end());
        const u32 component = (*comparison)[2];
        const u32 boundary = constants.at((*comparison)[3]);
        const auto masks = Instructions(code, 199); // OpBitwiseAnd
        const auto mask =
            std::ranges::find_if(masks, [=](const auto& words) { return words[1] == component; });
        REQUIRE(mask != masks.end());
        const u32 pair = (*mask)[2];
        const u32 pair_mask = constants.at((*mask)[3]);
        REQUIRE(pair_mask == 3);
        REQUIRE(boundary == 3);
        const auto loads = Instructions(code, 61); // OpLoad
        const auto pair_load =
            std::ranges::find_if(loads, [=](const auto& words) { return words[1] == pair; });
        REQUIRE(pair_load != loads.end());
        const u32 counter = (*pair_load)[2];
        const auto stores = Instructions(code, 62);     // OpStore
        const auto additions = Instructions(code, 128); // OpIAdd
        u32 initializations = 0;
        u32 increments = 0;
        for (const auto& store : stores) {
            if (store[0] != counter) {
                continue;
            }
            if (constants.contains(store[1])) {
                REQUIRE(constants.at(store[1]) == 0);
                ++initializations;
            } else {
                const auto increment = std::ranges::find_if(
                    additions, [&](const auto& words) { return words[1] == store[1]; });
                REQUIRE(increment != additions.end());
                REQUIRE((*increment)[2] == pair);
                REQUIRE(constants.at((*increment)[3]) == 1);
                ++increments;
            }
        }
        REQUIRE(initializations == 1);
        REQUIRE(increments == 1);                    // No lane-dependent input controls rounding.
        const auto limits = Instructions(code, 176); // OpULessThan
        const auto limit =
            std::ranges::find_if(limits, [=](const auto& words) { return words[2] == pair; });
        REQUIRE(limit != limits.end());
        const u32 pairs = constants.at((*limit)[3]);
        REQUIRE(pairs == depth / 2);
        std::vector<u32> rounded_pairs;
        for (u32 value = 0; value < pairs; ++value) {
            if ((value & pair_mask) == boundary) {
                rounded_pairs.push_back(value);
            }
        }
        REQUIRE(rounded_pairs == (depth == 8 ? std::vector<u32>{3} : std::vector<u32>{3, 7}));

        // Inspect the generated function, not a second implementation of its branch.
        // Every expensive conversion is inside the true block; no collective is.
        u32 block = 0;
        u32 rounding_operations = 0;
        u32 fma_operations = 0;
        u32 shuffles = 0;
        std::vector<u32> rounded_accumulators;
        bool round_rejoins = false;
        for (size_t offset = 5; offset < code.size();) {
            const u32 length = code[offset] >> 16;
            const u32 opcode = code[offset] & 0xffffU;
            REQUIRE(length != 0);
            REQUIRE(offset + length <= code.size());
            const auto words = std::span{code}.subspan(offset + 1, length - 1);
            if (opcode == 248) { // OpLabel
                block = words[0];
            } else if (opcode == 12 && words[3] == 2) { // GLSL.std.450 RoundEven
                REQUIRE(block == round_block);
                ++rounding_operations;
            } else if (opcode == 12 && words[3] == 50) { // GLSL.std.450 Fma
                REQUIRE(block != round_block);
                ++fma_operations;
            } else if (opcode == 345) { // OpGroupNonUniformShuffle
                REQUIRE(block != round_block);
                ++shuffles;
            } else if (opcode == 62 && block == round_block) { // OpStore
                rounded_accumulators.push_back(words[0]);
            } else if (opcode == 249 && block == round_block) { // OpBranch
                REQUIRE(words[0] == merge);
                round_rejoins = true;
            } else if (opcode == 169) { // OpSelect must not evaluate a discarded conversion.
                REQUIRE(words[2] != condition);
            }
            offset += length;
        }
        REQUIRE(rounding_operations == 4);
        REQUIRE(fma_operations == 8);
        REQUIRE(shuffles == 4);
        REQUIRE(round_rejoins);
        REQUIRE(rounded_accumulators.size() == 4);
        std::ranges::sort(rounded_accumulators);
        REQUIRE(std::ranges::adjacent_find(rounded_accumulators) == rounded_accumulators.end());
    }
}

TEST_CASE("Software matrix helpers also emit for 64-lane host subgroups",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.profile.max_subgroup_size = 64;
        const auto code = fixture.Emit(depth == 8 ? "software-k8-wave64" : "software-k16-wave64");
        REQUIRE_FALSE(HasCapability(code, 5357));
        REQUIRE(Instructions(code, 246).size() == 1);
        REQUIRE(Instructions(code, 345).size() == 4); // OpGroupNonUniformShuffle
    }
}

TEST_CASE("Compact HMMA lowering emits one helper call per matrix multiply",
          "[video_core][tensor][ampere]") {
    // HMMA.16816.F16 R44, R16, R48, RZ (Donkey Kong Bananza capture) and EXIT.
    constexpr Ampere::Instruction Hmma{{0x102c723c, 0x30, 0x8ff, 0x04aff000}};
    constexpr Ampere::Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
    const std::array code{Hmma, Exit};
    const auto is_tensor = [](u64 word) { return (word >> 51) == (0x5d30000000000000ULL >> 51); };

    const auto expanded = Ampere::LowerToMaxwell(code, {.tensor = true});
    INFO(expanded.error);
    REQUIRE(static_cast<bool>(expanded));
    REQUIRE(std::ranges::none_of(expanded.instructions, is_tensor));

    const auto compact = Ampere::LowerToMaxwell(code, {.tensor = true, .compact_tensor = true});
    INFO(compact.error);
    REQUIRE(static_cast<bool>(compact));
    REQUIRE(std::ranges::count_if(compact.instructions, is_tensor) == 1);
    const u64 word = *std::ranges::find_if(compact.instructions, is_tensor);
    CHECK((word & 0xff) == 0x2c);
    CHECK(((word >> 8) & 0xff) == 0x10);
    CHECK(((word >> 20) & 0xff) == 0x30);
    CHECK(((word >> 39) & 0xff) == 0xff);
    CHECK(((word >> 48) & 1) == 1);
    CHECK(((word >> 49) & 1) == 0);
    CHECK(compact.instructions.size() * 4 < expanded.instructions.size());

    // A guarded HMMA keeps the expansion: the helper shuffles across the whole warp.
    auto guarded = code;
    guarded[0].words[0] = (guarded[0].words[0] & ~0xf000U) | (1U << 12);
    const auto guarded_lowered =
        Ampere::LowerToMaxwell(guarded, {.tensor = true, .compact_tensor = true});
    INFO(guarded_lowered.error);
    REQUIRE(static_cast<bool>(guarded_lowered));
    CHECK(std::ranges::none_of(guarded_lowered.instructions, is_tensor));
}
