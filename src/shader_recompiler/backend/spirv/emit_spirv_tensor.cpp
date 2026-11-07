// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
namespace {
bool HasUniformMatrixCalls(const IR::Program& program) {
    // Deliberately do not infer uniformity from a preceding barrier or a full subgroup.
    // Only top-level calls qualify; even uniform loops before the last call fall back for now.
    // Divergent returns after the final helper call cannot affect any staging-memory barrier.
    const IR::AbstractSyntaxNode* last_call{};
    for (const auto& node : program.syntax_list) {
        if (node.type != IR::AbstractSyntaxNode::Type::Block) continue;
        for (const auto& inst : node.data.block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::TensorMulAdd) last_call = &node;
        }
    }
    if (!last_call) return false;
    u32 nesting{};
    for (const auto& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block:
            for (const auto& inst : node.data.block->Instructions()) {
                if (nesting && inst.GetOpcode() == IR::Opcode::TensorMulAdd) return false;
            }
            if (&node == last_call) return !nesting;
            break;
        case IR::AbstractSyntaxNode::Type::If:
            ++nesting;
            break;
        case IR::AbstractSyntaxNode::Type::EndIf:
            if (!nesting) return false;
            --nesting;
            break;
        default:
            return false;
        }
    }
    return false;
}
} // namespace

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) {
                continue;
            }
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) {
                throw LogicError("Invalid software HMMA depth");
            }
            needed[depth == 16] = true;
        }
    }
    const char* shared_option = std::getenv("EDEN_NX2_HMMA_SHARED_B");
    const u64 threads = static_cast<u64>(program.workgroup_size[0]) *
                        program.workgroup_size[1] * program.workgroup_size[2];
    const u64 staging_offset = (static_cast<u64>(program.shared_memory_size) + 15) & ~15ULL;
    // SPIR-V 1.6, local X divisible by required32 guarantee full subgroups without a pipeline flag.
    // Full subgroups alone do not guarantee uniform helper calls: check the structured program too.
    // Keep total explicit Workgroup storage within Vulkan's guaranteed 16 KiB minimum.
    const bool shared_b = (needed[0] || needed[1]) && shared_option &&
                          std::string_view{shared_option} == "1" &&
                          program.stage == Stage::Compute &&
                          ctx.profile.required_compute_subgroup_size == 32 &&
                          ctx.profile.support_subgroup_broadcast &&
                          ctx.profile.prefer_software_matrix_broadcast &&
                          ctx.profile.support_explicit_workgroup_layout &&
                          ctx.profile.supported_spirv >= 0x00010600 &&
                          (!program.shared_memory_size || ctx.uses_explicit_workgroup_layout) &&
                          program.workgroup_size[0] % 32 == 0 && threads && threads <= 1024 &&
                          staging_offset + threads * sizeof(u32) <= 16384 &&
                          HasUniformMatrixCalls(program);
    Id staging{}, staging_element{}, subgroup_id{};
    if (shared_b) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        const Id array = ctx.TypeArray(ctx.U32[1], ctx.Const(static_cast<u32>(threads)));
        ctx.Decorate(array, spv::Decoration::ArrayStride, 4U);
        const Id block = ctx.TypeStruct(array);
        ctx.MemberDecorate(block, 0U, spv::Decoration::Offset, static_cast<u32>(staging_offset));
        ctx.Decorate(block, spv::Decoration::Block);
        // Explicit-layout blocks share an address space. Reserve space beyond all guest views.
        staging = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Workgroup, block),
                                        spv::StorageClass::Workgroup);
        ctx.Decorate(staging, spv::Decoration::Aliased);
        staging_element = ctx.TypePointer(spv::StorageClass::Workgroup, ctx.U32[1]);
        subgroup_id = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]),
                                            spv::StorageClass::Input);
        ctx.Decorate(subgroup_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupId);
        ctx.interfaces.push_back(staging);
        ctx.interfaces.push_back(subgroup_id);
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) {
            continue;
        }
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.profile.SupportsSubgroupStage(program.stage) ||
            ctx.profile.has_broken_spirv_subgroup_shuffle)
            throw NotImplementedException("Software HMMA requires subgroup shuffle support");
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal =
                ctx.OpSelect(u32ty, ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)),
                             imm(0x7f800000), rounded);
            const Id scaled =
                ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(
                u32ty, ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite =
                ctx.OpSelect(u32ty, ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)),
                             normal, subnormal);
            const Id result =
                ctx.OpSelect(u32ty, ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)),
                             magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.software_matrix_functions[variant] = fn;
        const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
        const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
        const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                                spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                       spv::StorageClass::Function);
        std::array<Id, 8> inputs;
        for (u32 i = 0; i < 4; ++i)
            inputs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
        for (u32 i = 0; i < 2; ++i) {
            inputs[4 + i] = ctx.OpCompositeExtract(u32ty, matrix_b, i);
            inputs[6 + i] = ctx.OpCompositeExtract(u32ty, matrix_c, i);
        }
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
        const Id warp_base = and_(lane, ~31U);
        const Id row_base = add(warp_base, and_(lane, 28));
        const Id col_base = add(warp_base, ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3)));
        const Id shared_base = shared_b
            ? ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, subgroup_id), imm(32)) : Id{};
        // A comes from lane `component` of this lane's own quad: a quad broadcast with a constant
        // index, which hosts do without a general (LDS) shuffle.
        const bool quad = ctx.profile.support_quad_shuffles;
        if (quad) {
            ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
        }
        const bool broadcast_b = program.stage == Stage::Compute &&
                                 ctx.profile.required_compute_subgroup_size == 32 &&
                                 ctx.profile.support_subgroup_broadcast &&
                                 ctx.profile.prefer_software_matrix_broadcast;
        if (broadcast_b) {
            ctx.AddCapability(spv::Capability::GroupNonUniformBallot);
        }
        const auto gather_b = [&](Id value, u32 offset) {
            if (shared_b) {
                const Id index = add(shared_base, add(col_base, imm(offset)));
                return ctx.OpLoad(u32ty, ctx.OpAccessChain(staging_element, staging, imm(0), index));
            }
            if (!broadcast_b) {
                return ctx.OpGroupNonUniformShuffle(u32ty, imm(3), value,
                                                   add(col_base, imm(offset)));
            }
            // Turnip waterfalls divergent relative shuffle offsets over all 32 lanes.
            // These gathers have only four absolute sources; keep every broadcast unconditional.
            std::array<Id, 4> sources;
            for (u32 i = 0; i < sources.size(); ++i) {
                sources[i] = ctx.OpGroupNonUniformBroadcast(u32ty, imm(3), value,
                                                           imm(offset + i * 8));
            }
            const Id column = and_(lane, 3);
            const Id low = ctx.OpSelect(u32ty, ctx.OpIEqual(ctx.U1, column, imm(0)),
                                       sources[0], sources[1]);
            const Id high = ctx.OpSelect(u32ty, ctx.OpIEqual(ctx.U1, column, imm(2)),
                                        sources[2], sources[3]);
            return ctx.OpSelect(u32ty, ctx.OpULessThan(ctx.U1, column, imm(2)), low, high);
        };
        // One iteration per K=8 group (the four K pairs between roundings), unrolled so every
        // pair's lanes are constants. Unrolling all of K=16 instead lets the host compiler hoist
        // the second group's shuffles, and large networks then spill registers.
        const u32 groups = variant ? 2 : 1;
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(),
                 end = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id group = ctx.OpLoad(u32ty, counter);
        const Id more = ctx.OpULessThan(ctx.U1, group, imm(groups));
        ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
        ctx.OpBranchConditional(more, body, end);
        ctx.AddLabel(body);

        const Id upper = ctx.OpUGreaterThanEqual(ctx.U1, group, imm(1));
        const auto pick = [&](u32 low, u32 high) {
            return groups == 1 ? inputs[low] : ctx.OpSelect(u32ty, upper, inputs[high], inputs[low]);
        };
        const Id b = pick(4, 5);
        std::array<Id, 2> a_regs{pick(0, 2), pick(1, 3)};
        if (shared_b) {
            // The first barrier prevents reuse racing the preceding K8 step or helper call.
            const auto barrier = [&] {
                ctx.OpControlBarrier(imm(3), imm(3), imm(264)); // Subgroup, AcquireRelease|Workgroup
            };
            barrier();
            ctx.OpStore(ctx.OpAccessChain(staging_element, staging, imm(0), add(shared_base, lane)), b);
            barrier();
        }
        for (u32 component = 0; component < 4; ++component) {
            std::array<Id, 2> bs;
            for (u32 col = 0; col < 2; ++col)
                bs[col] = unpack(gather_b(b, component + col * 4));
            for (u32 row = 0; row < 2; ++row) {
                const Id shuffled =
                    quad ? ctx.OpGroupNonUniformQuadBroadcast(u32ty, imm(3), a_regs[row],
                                                              imm(component))
                         : ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a_regs[row],
                                                        add(row_base, imm(component)));
                const Id as = unpack(shuffled);
                for (u32 col = 0; col < 2; ++col) {
                    Id value = ctx.OpLoad(f32ty, accumulators[row * 2 + col]);
                    for (u32 half = 0; half < 2; ++half) {
                        value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                          ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                        ctx.Decorate(value, spv::Decoration::NoContraction);
                    }
                    ctx.OpStore(accumulators[row * 2 + col], value);
                }
            }
        }
        // HMMA.F16 rounds after each K=8 group, including the midpoint of K=16.
        for (const Id accumulator : accumulators) {
            ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        }
        ctx.OpBranch(next);
        ctx.AddLabel(next);
        ctx.OpStore(counter, add(group, imm(1)));
        ctx.OpBranch(header);
        ctx.AddLabel(end);
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(
                u32ty,
                ctx.OpCompositeConstruct(ctx.F32[2], ctx.OpLoad(f32ty, accumulators[row * 2]),
                                         ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16], a, b, c);
}

} // namespace Shader::Backend::SPIRV
