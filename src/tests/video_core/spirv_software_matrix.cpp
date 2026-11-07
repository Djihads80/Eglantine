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
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using namespace Shader;

struct TensorMode {
    const char* key;
    std::string previous;
    bool existed{};

    explicit TensorMode(const char* mode, const char* variable = "EDEN_NX2_TENSOR") : key{variable} {
        if (const char* value = std::getenv(key)) {
            previous = value;
            existed = true;
        }
        Set(mode, key);
    }

    static void Set(const char* value, const char* variable = "EDEN_NX2_TENSOR") {
#ifdef _WIN32
        _putenv_s(variable, value ? value : "");
#else
        if (value) {
            setenv(variable, value, 1);
        } else {
            unsetenv(variable);
        }
#endif
    }

    ~TensorMode() {
        Set(existed ? previous.c_str() : nullptr, key);
    }
};

struct TensorProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit TensorProgram(u32 count = 1, u32 software_depth = 0, bool store_output = true,
                           bool gpu_fixture = false) {
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
        IR::U32 global;
        if (gpu_fixture) {
            program.workgroup_size = {128, 1, 1};
            program.info.uses_workgroup_id = true;
            program.info.uses_local_invocation_id = true;
            program.info.used_storage_buffer_types = IR::Type::U32;
            program.info.storage_buffers_descriptors = {{0, 0, 1, false}, {0, 16, 1, true}};
            global = ir.IAdd(ir.ShiftLeftLogical(ir.WorkgroupIdX(), ir.Imm32(7)),
                             ir.LocalInvocationIdX());
        }
        std::array<IR::U32, 8> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            if (gpu_fixture) {
                const auto offset = ir.IAdd(ir.ShiftLeftLogical(global, ir.Imm32(5)), ir.Imm32(i * 4));
                inputs[i] = IR::U32{&*block.PrependNewInst(block.end(), IR::Opcode::LoadStorage32,
                                                        {ir.Imm32(0), offset})};
            } else {
                inputs[i] = software_depth ? ir.IAdd(ir.Imm32(0x3c003c00U + i * 0x01000100U),
                ir.ShiftLeftLogical(lane, ir.Imm32(3))) : ir.IAdd(ir.Imm32((i + 1) * 0x01000100U), lane);
            }
        }
        IR::U32 shared_offset;
        if (gpu_fixture) {
            shared_offset = ir.ShiftLeftLogical(ir.LocalInvocationIdX(), ir.Imm32(2));
            block.AppendNewInst(IR::Opcode::WriteSharedU32, {shared_offset, inputs[0]});

        }
        for (u32 i = 0; i < count; ++i) {
            const auto result =
                ir.TensorMulAdd(ir.CompositeConstruct(inputs[0], inputs[1], inputs[2], inputs[3]),
                                ir.CompositeConstruct(inputs[4], inputs[5]),
                                ir.CompositeConstruct(inputs[6], inputs[7]), software_depth);
            inputs[6] = IR::U32{ir.CompositeExtract(result, 0)};
            inputs[7] = IR::U32{ir.CompositeExtract(result, 1)};
        }
        if (software_depth && store_output) {
            program.info.used_storage_buffer_types = IR::Type::U32;
            if (!gpu_fixture) program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
            program.info.uses_local_invocation_id = true;
            const auto offset = ir.ShiftLeftLogical(gpu_fixture ? global : ir.LocalInvocationIdX(), ir.Imm32(3));
            if (gpu_fixture) {
                const IR::U32 saved{&*block.PrependNewInst(block.end(), IR::Opcode::LoadSharedU32,
                                                        {shared_offset})};
                inputs[6] = ir.BitwiseXor(inputs[6], saved);
            }
            const auto output_binding = ir.Imm32(gpu_fixture ? 1 : 0);
            block.AppendNewInst(IR::Opcode::WriteStorage32, {output_binding, offset, inputs[6]});
            block.AppendNewInst(IR::Opcode::WriteStorage32,
                {output_binding, ir.IAdd(offset, ir.Imm32(4)), inputs[7]});

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

struct HelperCounts {
    u32 fma{};
    u32 shuffles{};
    u32 quad_broadcasts{};
    u32 conversions{};
    std::vector<u32> fma_before_rounding; ///< FMAs emitted before each RoundEven
    std::vector<u32> quad_indices;
    std::vector<u32> conversion_ids;
};

HelperCounts CountHelper(const std::vector<u32>& code) {
    std::map<u32, u32> constants;
    for (const auto& words : Instructions(code, 43)) { // OpConstant
        constants.emplace(words[1], words[2]);
    }
    HelperCounts counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 length = code[offset] >> 16;
        const u32 opcode = code[offset] & 0xffffU;
        REQUIRE(length != 0);
        REQUIRE(offset + length <= code.size());
        const auto words = std::span{code}.subspan(offset + 1, length - 1);
        if (opcode == 12 && words[3] == 2) { // GLSL.std.450 RoundEven
            counts.fma_before_rounding.push_back(counts.fma);
        } else if (opcode == 12 && words[3] == 50) { // GLSL.std.450 Fma
            ++counts.fma;
        } else if (opcode == 345) { // OpGroupNonUniformShuffle
            ++counts.shuffles;
        } else if (opcode == 365) { // OpGroupNonUniformQuadBroadcast
            ++counts.quad_broadcasts;
            REQUIRE(constants.contains(words[4]));
            counts.quad_indices.push_back(constants.at(words[4]));
        } else if (opcode == 115) { // OpFConvert
            ++counts.conversions;
            counts.conversion_ids.push_back(words[1]);
        }
        offset += length;
    }
    return counts;
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

TEST_CASE("Software HMMA rounds once per K8 group", "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool quad : {false, true}) {
            TensorProgram fixture{1, depth};
            fixture.profile.support_quad_shuffles = quad;
            const auto code = fixture.Emit();
            std::map<u32, u32> constants;
            for (const auto& words : Instructions(code, 43)) { // OpConstant
                constants.emplace(words[1], words[2]);
            }
            REQUIRE(Instructions(code, 247).empty()); // OpSelectionMerge: no lane-dependent branch
            REQUIRE(HasCapability(code, 68) == quad); // GroupNonUniformQuad
            // The loop runs once per K8 group.
            const auto limits = Instructions(code, 176); // OpULessThan
            REQUIRE(limits.size() == 1);
            REQUIRE(constants.at(limits.front()[3]) == depth / 8);
            const HelperCounts counts = CountHelper(code);
            // Four K pairs x 2 rows x 2 columns x 2 halves, then the four accumulators rounded.
            REQUIRE(counts.fma == 32);
            REQUIRE(counts.fma_before_rounding == std::vector<u32>(4, 32));
            // B always needs a general shuffle (two columns); A comes from its own quad.
            REQUIRE(counts.shuffles == (quad ? 8U : 16U));
            REQUIRE(counts.quad_broadcasts == (quad ? 8U : 0U));
            for (u32 i = 0; i < counts.quad_indices.size(); ++i) {
                REQUIRE(counts.quad_indices[i] == i / 2);
            }
        }
    }
}

TEST_CASE("Software matrix helpers also emit for 64-lane host subgroups",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.profile.max_subgroup_size = 64;
        fixture.profile.warp_size_potentially_larger_than_guest = true;
        const auto code = fixture.Emit(depth == 8 ? "software-k8-wave64" : "software-k16-wave64");
        REQUIRE_FALSE(HasCapability(code, 5357));
        REQUIRE(Instructions(code, 246).size() == 1);
        REQUIRE(Instructions(code, 345).size() == 16); // OpGroupNonUniformShuffle
    }
}

TEST_CASE("Software matrix broadcasts require a supported 32-lane compute pipeline",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool quad : {false, true}) {
            for (const u32 required_size : {0U, 32U, 64U}) {
                for (const bool supported : {false, true}) {
                    for (const bool preferred : {false, true}) {
                        TensorProgram fixture{1, depth};
                        fixture.profile.max_subgroup_size = 64;
                        fixture.profile.warp_size_potentially_larger_than_guest = true;
                        fixture.profile.required_compute_subgroup_size = required_size;
                        fixture.profile.support_subgroup_broadcast = supported;
                        fixture.profile.prefer_software_matrix_broadcast = preferred;
                        fixture.profile.support_quad_shuffles = quad;
                        const bool optimized = required_size == 32 && supported && preferred;
                        const auto code = fixture.Emit(
                            "software-guard-k" + std::to_string(depth) + "-quad" +
                            std::to_string(quad) + "-required" + std::to_string(required_size) +
                            "-supported" + std::to_string(supported) + "-preferred" +
                            std::to_string(preferred));
                        const auto counts = CountHelper(code);
                        CAPTURE(depth, quad, required_size, supported, preferred);
                        REQUIRE(Instructions(code, 337).size() == (optimized ? 32U : 0U));
                        REQUIRE(counts.shuffles == (quad ? 8U : 16U) - (optimized ? 8U : 0U));
                        REQUIRE(counts.quad_broadcasts == (quad ? 8U : 0U));
                        REQUIRE(HasCapability(code, 64) == optimized);
                        REQUIRE(HasCapability(code, 65)); // GroupNonUniformShuffle
                        REQUIRE(HasCapability(code, 68) == quad);
                        REQUIRE(counts.fma == 32);
                        REQUIRE(counts.fma_before_rounding == std::vector<u32>(4, 32));
                        REQUIRE(Instructions(code, 246).size() == 1);
                        REQUIRE(Instructions(code, 247).empty());
                    }
                }
            }
        }
    }
}

TEST_CASE("Software matrix broadcast routing preserves every B source lane",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.support_quad_shuffles = true;
        const auto code = fixture.Emit(depth == 8 ? "software-k8-broadcast" : "software-k16-broadcast");
        std::map<u32, std::vector<u32>> definitions;
        u32 lane_variable{};
        std::vector<u32> gathers;
        for (size_t offset = 5; offset < code.size();) {
            const u32 size = code[offset] >> 16;
            REQUIRE(size != 0);
            REQUIRE(offset + size <= code.size());
            const auto words = std::span{code}.subspan(offset, size);
            const auto opcode = static_cast<spv::Op>(words[0] & 0xffffU);
            bool has_result{}, has_type{};
            spv::HasResultAndType(opcode, &has_result, &has_type);
            if (has_result && has_type) {
                definitions.emplace(words[2], std::vector<u32>{words.begin(), words.end()});
            }
            if (opcode == spv::Op::OpDecorate && words[2] == 11 && words[3] == 41) {
                lane_variable = words[1];
            }
            if (opcode == spv::Op::OpExtInst && words[4] == 62) {
                const auto found = definitions.find(words[5]);
                if (found != definitions.end() &&
                    (found->second[0] & 0xffffU) == static_cast<u32>(spv::Op::OpSelect)) {
                    gathers.push_back(words[5]);
                }
            }
            offset += size;
        }
        REQUIRE(lane_variable != 0);
        REQUIRE(gathers.size() == 8);
        const auto broadcasts = Instructions(code, 337);
        REQUIRE(broadcasts.size() == 32);
        const auto constants = Instructions(code, 43);
        for (u32 i = 0; i < broadcasts.size(); ++i) {
            REQUIRE(broadcasts[i][3] == broadcasts.front()[3]);
            const auto constant = std::ranges::find_if(constants, [&](const auto& words) {
                return words[1] == broadcasts[i][4];
            });
            REQUIRE(constant != constants.end());
            const u32 gather = i / 4;
            CHECK((*constant)[2] == gather / 2 + (gather % 2) * 4 + (i % 4) * 8);
        }
        const auto evaluate = [&](auto&& self, u32 id, u32 lane) -> u32 {
            const auto& words = definitions.at(id);
            const auto arg = [&](size_t index) { return self(self, words[index], lane); };
            switch (static_cast<spv::Op>(words[0] & 0xffffU)) {
            case spv::Op::OpConstant: return words[3];
            case spv::Op::OpLoad:
                REQUIRE(words[3] == lane_variable);
                return lane;
            case spv::Op::OpBitwiseAnd: return arg(3) & arg(4);
            case spv::Op::OpIEqual: return arg(3) == arg(4);
            case spv::Op::OpULessThan: return arg(3) < arg(4);
            case spv::Op::OpSelect: return arg(3) ? arg(4) : arg(5);
            case spv::Op::OpGroupNonUniformBroadcast:
                REQUIRE((definitions.at(words[5])[0] & 0xffffU) ==
                        static_cast<u32>(spv::Op::OpConstant));
                return arg(5);
            default: FAIL("Unexpected B gather opcode " << (words[0] & 0xffffU));
            }
            return 0;
        };
        for (u32 lane = 0; lane < 32; ++lane) {
            for (u32 i = 0; i < gathers.size(); ++i) {
                CAPTURE(depth, lane, i);
                CHECK(evaluate(evaluate, gathers[i], lane) ==
                      8 * (lane & 3) + i / 2 + (i % 2) * 4);
            }
        }
        fixture.program.info.uses_fp16 = true;
        const auto fp16_code = fixture.Emit(depth == 8 ? "software-k8-broadcast-fp16"
                                                      : "software-k16-broadcast-fp16");
        const auto counts = CountHelper(fp16_code);
        REQUIRE(counts.fma == 32);
        REQUIRE(counts.conversions == 8);
        REQUIRE(counts.fma_before_rounding.empty());
        const auto decorations = Instructions(fp16_code, 71);
        for (const u32 id : counts.conversion_ids) {
            REQUIRE(std::ranges::any_of(decorations, [=](const auto& words) {
                return words[0] == id && words[1] == 42;
            }));
        }
        TensorProgram repeated{64, depth};
        repeated.profile = fixture.profile;
        const auto repeated_code = repeated.Emit(depth == 8 ? "software-k8-broadcast-many"
                                                           : "software-k16-broadcast-many");
        REQUIRE(Instructions(repeated_code, 337).size() == 32);
        REQUIRE(Instructions(repeated_code, 54).size() == 2);
        REQUIRE(Instructions(repeated_code, 57).size() == 64);
        REQUIRE(Instructions(repeated_code, 246).size() == 1);
        REQUIRE(repeated_code.size() < code.size() + 64 * 40);
    }
}

TEST_CASE("Software matrix compute broadcast policy leaves other stages unchanged",
          "[video_core][tensor][ampere]") {
    TensorProgram fixture{1, 16, false};
    fixture.program.stage = Stage::Fragment;
    fixture.program.info.uses_local_invocation_id = false;
    fixture.profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Fragment);
    fixture.profile.required_compute_subgroup_size = 32;
    fixture.profile.support_subgroup_broadcast = true;
    fixture.profile.prefer_software_matrix_broadcast = true;
    const auto code = fixture.Emit("software-fragment-fallback");
    REQUIRE(Instructions(code, 337).empty());
    REQUIRE(Instructions(code, 345).size() == 16);
}

TEST_CASE("Software matrix broadcast preference preserves subgroup admission checks",
          "[video_core][tensor][ampere]") {
    for (const bool broken : {false, true}) {
        TensorProgram fixture{1, 16};
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.has_broken_spirv_subgroup_shuffle = broken;
        if (!broken) {
            fixture.profile.supported_subgroup_stages = 0;
        }
        REQUIRE_THROWS_AS(fixture.Emit(), NotImplementedException);
    }
}

TEST_CASE("Software matrix shared B is opt-in and keeps a separate explicit-layout block",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool guest_shared : {false, true}) {
            for (const char* option : {"0", "1", "true"}) {
                TensorMode mode{option, "EDEN_NX2_HMMA_SHARED_B"};
                TensorProgram fixture{64, depth};
                fixture.profile.supported_spirv = 0x00010600;
                fixture.profile.required_compute_subgroup_size = 32;
                fixture.profile.support_subgroup_broadcast = true;
                fixture.profile.prefer_software_matrix_broadcast = true;
                fixture.profile.support_quad_shuffles = true;
                fixture.profile.support_explicit_workgroup_layout = true;
                fixture.program.shared_memory_size = guest_shared ? 4096 : 0;
                const bool enabled = std::string_view{option} == "1";
                const auto code = fixture.Emit("software-shared-k" + std::to_string(depth) +
                    "-guest" + std::to_string(guest_shared) + "-opt" + option);
                const auto barriers = Instructions(code, 224);
                REQUIRE(barriers.size() == (enabled ? 2U : 0U));
                REQUIRE(Instructions(code, 337).size() == (enabled ? 0U : 32U));
                REQUIRE(Instructions(code, 345).empty());
                REQUIRE(Instructions(code, 57).size() == 64);
                REQUIRE(CountHelper(code).fma == 32);
                if (!enabled) continue;
                std::map<u32, u32> constants;
                for (const auto& words : Instructions(code, 43)) constants[words[1]] = words[2];
                for (const auto& words : barriers) {
                    REQUIRE(constants.at(words[0]) == 3);
                    REQUIRE(constants.at(words[1]) == 3);
                    REQUIRE(constants.at(words[2]) == 264);
                }
                const auto variables = Instructions(code, 59);
                const auto decorations = Instructions(code, 71);
                std::map<u32, u32> pointers, offsets;
                for (const auto& p : Instructions(code, 32)) pointers[p[0]] = p[2];
                for (const auto& m : Instructions(code, 72))
                    if (m[1] == 0 && m[2] == 35) offsets[m[0]] = m[3];
                u32 scratch{};
                for (const auto& words : variables) {
                    if (words[2] != 4) continue; // Workgroup
                    const bool aliased = std::ranges::any_of(decorations, [&](const auto& d) {
                        return d[0] == words[1] && d[1] == 20;
                    });
                    REQUIRE(aliased);
                    if (offsets.at(pointers.at(words[0])) == (guest_shared ? 4096U : 0U)) {
                        REQUIRE(scratch == 0);
                        scratch = words[1];
                    }
                }
                REQUIRE(scratch != 0);
                u32 loads{}, stores{};
                for (const auto& chain : Instructions(code, 65)) {
                    if (chain[2] != scratch) continue;
                    REQUIRE(chain.size() == 5); // block member zero, then array index
                    REQUIRE(constants.at(chain[3]) == 0);
                    for (const auto& load : Instructions(code, 61)) loads += load[2] == chain[1];
                    for (const auto& store : Instructions(code, 62)) stores += store[0] == chain[1];
                }
                REQUIRE(loads == 8);
                REQUIRE(stores == 1);
            }
        }
    }
}

TEST_CASE("Software matrix shared B rejects partial subgroups and uncertain call flow",
          "[video_core][tensor][ampere]") {
    TensorMode mode{"1", "EDEN_NX2_HMMA_SHARED_B"};
    for (u32 reason = 0; reason < 13; ++reason) {
        TensorProgram fixture{1, 16};
        fixture.profile.supported_spirv = 0x00010600;
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.support_explicit_workgroup_layout = true;
        IR::Block preheader{fixture.pool}, merge{fixture.pool};
        switch (reason) {
        case 0: fixture.program.workgroup_size = {48, 1, 1}; break;
        case 1: fixture.profile.required_compute_subgroup_size = 64; break;
        case 2: fixture.profile.support_explicit_workgroup_layout = false; break;
        case 3: fixture.program.shared_memory_size = 16384; break;
        case 4: fixture.profile.prefer_software_matrix_broadcast = false; break;
        case 5: fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(),
                    {{}, IR::AbstractSyntaxNode::Type::Return}); break;
        case 6: fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(),
                    {{}, IR::AbstractSyntaxNode::Type::Unreachable}); break;
        case 7: fixture.program.workgroup_size = {32, 0, 1}; break;
        case 8: fixture.profile.support_subgroup_broadcast = false; break;
        case 9:
            fixture.program.blocks = {&preheader, &fixture.block, &merge};
            fixture.program.post_order_blocks = fixture.program.blocks;
            fixture.program.syntax_list = {
                {{.block = &preheader}, IR::AbstractSyntaxNode::Type::Block},
                {{.if_node = {IR::U1{IR::Value{true}}, &fixture.block, &merge}}, IR::AbstractSyntaxNode::Type::If},
                {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
                {{.end_if = {&merge}}, IR::AbstractSyntaxNode::Type::EndIf},
                {{.block = &merge}, IR::AbstractSyntaxNode::Type::Block},
                {{}, IR::AbstractSyntaxNode::Type::Return}};
            break;
        case 10: fixture.profile.supported_spirv = 0x00010500; break;
        case 11:
            fixture.program.shared_memory_size = 6912;
            fixture.program.info.uses_int16 = true;
            fixture.profile.support_workgroup_layout_16bit_access = false;
            break;
        case 12: fixture.program.workgroup_size = {1056, 1, 1}; break;
        }
        // Invalid syntax/zero-sized workgroups are admission-only checks, not valid modules.
        const auto code = fixture.Emit();
        CAPTURE(reason);
        REQUIRE(Instructions(code, 224).empty());
        REQUIRE(Instructions(code, 337).size() == ((reason == 1 || reason == 4 || reason == 8) ? 0U : 32U));
    }
}

TEST_CASE("Software matrix source GPU fixtures preserve guest shared memory",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const u32 count : {1U, 192U}) {
            for (const bool fp16 : {false, true}) {
                for (const char* option : {"0", "1"}) {
                    TensorMode mode{option, "EDEN_NX2_HMMA_SHARED_B"};
                    TensorProgram fixture{count, depth, true, true};
                    fixture.profile.supported_spirv = 0x00010600;
                    fixture.profile.required_compute_subgroup_size = 32;
                    fixture.profile.support_subgroup_broadcast = true;
                    fixture.profile.prefer_software_matrix_broadcast = true;
                    fixture.profile.support_quad_shuffles = true;
                    fixture.profile.support_explicit_workgroup_layout = true;
                    fixture.profile.support_workgroup_layout_16bit_access = true;
                    fixture.profile.support_int16 = true;
                    fixture.program.info.uses_int16 = true;
                    fixture.program.info.uses_fp16 = fp16;
                    fixture.program.shared_memory_size = 6912; // actual OoT shader guest allocation
                    const auto code = fixture.Emit("gpu-k" + std::to_string(depth) + "-n" +
                        std::to_string(count) + "-fp" + std::to_string(fp16) + "-opt" + option);
                    REQUIRE(Instructions(code, 224).size() == (option[0] == '1' ? 2U : 0U));
                }
            }
        }
    }
}

TEST_CASE("Software matrix shared B admits game-like reconverged calls and suffix returns",
          "[video_core][tensor][ampere]") {
    TensorMode mode{"1", "EDEN_NX2_HMMA_SHARED_B"};
    TensorProgram fixture{192, 16};
    fixture.profile.supported_spirv = 0x00010600;
    fixture.profile.required_compute_subgroup_size = 32;
    fixture.profile.support_subgroup_broadcast = true;
    fixture.profile.prefer_software_matrix_broadcast = true;
    fixture.profile.support_explicit_workgroup_layout = true;
    IR::Block preheader{fixture.pool}, optional{fixture.pool}, suffix{fixture.pool}, exit{fixture.pool};
    fixture.program.blocks = {&preheader, &optional, &fixture.block, &suffix, &exit};
    fixture.program.post_order_blocks = fixture.program.blocks;
    fixture.program.syntax_list = {
        {{.block = &preheader}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {IR::U1{IR::Value{true}}, &optional, &fixture.block}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &optional}, IR::AbstractSyntaxNode::Type::Block},
        {{.end_if = {&fixture.block}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {IR::U1{IR::Value{true}}, &suffix, &exit}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &suffix}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
        {{.end_if = {&exit}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &exit}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return}};
    const auto code = fixture.Emit("software-shared-game-flow");
    REQUIRE(Instructions(code, 224).size() == 2);
    REQUIRE(Instructions(code, 57).size() == 192);
}

TEST_CASE("Captured Bananza matrix shaders lower without an environment override",

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
