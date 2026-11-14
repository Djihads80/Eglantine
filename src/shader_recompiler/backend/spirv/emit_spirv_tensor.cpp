// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
namespace {

constexpr u32 GUEST_WARP_LANES = 32;
constexpr u32 LayoutRowMajor = 0;
constexpr u32 LayoutColumnMajor = 1;
// Subgroup scope, acquire-release on workgroup memory.
constexpr u32 SubgroupBarrierSemantics = 0x8 | 0x100;

// A cooperative matrix needs its whole subgroup: every row of the workgroup fills whole warps and
// the pipeline is created with full subgroups. Cooperative matrices also need the Vulkan memory
// model, which has no Coherent decoration, so a program that needs coherent memory keeps the
// emulation.
bool SupportsCooperativeHmma(const IR::Program& program) {
    return program.stage == Stage::Compute && program.workgroup_size[0] != 0 &&
           program.workgroup_size[0] % GUEST_WARP_LANES == 0 &&
           !program.info.requires_coherent_global_memory &&
           !program.info.requires_coherent_image_memory;
}

// Sirit lacks the cooperative matrix instructions. They are emitted as instructions with the same
// word layout, and PatchCooperativeHmma replaces the opcodes after assembly: OpTypeStruct for the
// types, OpSelect for the multiply-add, OpBitFieldUExtract for a load and a 5-word OpStore of a
// multiply-add result for a store. The type operands are constants that no real structure has as
// members. B and the accumulator are 16 rows by the given columns.
CooperativeHmmaTypes DefineCooperativeHmmaTypes(Sirit::Module& module, Id u32_type, u32 columns,
                                                u32 spirv_version) {
    module.AddExtension("SPV_KHR_cooperative_matrix");
    module.AddCapability(spv::Capability::CooperativeMatrixKHR);
    module.AddCapability(spv::Capability::Float16);
    if (spirv_version < 0x00010500) {
        module.AddExtension("SPV_KHR_vulkan_memory_model");
    }
    module.AddCapability(spv::Capability::VulkanMemoryModel);
    module.AddCapability(spv::Capability::VulkanMemoryModelDeviceScope);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::Vulkan);
    const Id float16{module.TypeFloat(16)};
    const auto constant{[&](u32 value) { return module.Constant(u32_type, value); }};
    const Id scope{constant(static_cast<u32>(spv::Scope::Subgroup))};
    const auto matrix{[&](u32 columns, spv::CooperativeMatrixUse use) {
        return module.TypeStruct(float16, scope, constant(16), constant(columns),
                                 constant(static_cast<u32>(use)));
    }};
    return CooperativeHmmaTypes{
        .float16 = float16,
        .float16x2 = module.TypeVector(float16, 2),
        .matrix_a = matrix(16, spv::CooperativeMatrixUse::MatrixAKHR),
        .matrix_b = matrix(columns, spv::CooperativeMatrixUse::MatrixBKHR),
        .matrix_c = matrix(columns, spv::CooperativeMatrixUse::MatrixAccumulatorKHR),
    };
}

// HMMA.16816.F16 on a cooperative matrix. Each lane already holds its share of the matrices in
// registers, so no data moves between lanes or through memory: the lane's halves become the
// matrix's local elements. Vulkan leaves their order to the driver; NVIDIA's is the guest's except
// that A's middle registers swap (see the self-test in vk_cooperative_hmma_test.cpp).
Id EmitCooperativeHmma(Sirit::Module& module, const CooperativeHmmaTypes& types, Id u32_type,
                       Id u32x2_type, Id a, Id b, Id c) {
    const auto insert_word{[&](Id matrix_type, Id matrix, Id vector, u32 word, u32 slot) {
        const Id halves{module.OpBitcast(types.float16x2,
                                         module.OpCompositeExtract(u32_type, vector, word))};
        for (u32 half = 0; half < 2; ++half) {
            matrix = module.OpCompositeInsert(
                matrix_type, module.OpCompositeExtract(types.float16, halves, half), matrix,
                slot * 2 + half);
        }
        return matrix;
    }};
    constexpr std::array<u32, 4> a_words{0, 2, 1, 3};
    Id matrix_a{module.OpUndef(types.matrix_a)};
    for (u32 slot = 0; slot < a_words.size(); ++slot) {
        matrix_a = insert_word(types.matrix_a, matrix_a, a, a_words[slot], slot);
    }
    Id matrix_b{module.OpUndef(types.matrix_b)};
    Id matrix_c{module.OpUndef(types.matrix_c)};
    for (u32 slot = 0; slot < 2; ++slot) {
        matrix_b = insert_word(types.matrix_b, matrix_b, b, slot, slot);
        matrix_c = insert_word(types.matrix_c, matrix_c, c, slot, slot);
    }
    // OpCooperativeMatrixMulAddKHR after PatchCooperativeHmma.
    const Id matrix_d{module.OpSelect(types.matrix_c, matrix_a, matrix_b, matrix_c)};
    const auto extract_word{[&](u32 word) {
        return module.OpBitcast(
            u32_type,
            module.OpCompositeConstruct(
                types.float16x2, module.OpCompositeExtract(types.float16, matrix_d, word * 2),
                module.OpCompositeExtract(types.float16, matrix_d, word * 2 + 1)));
    }};
    return module.OpCompositeConstruct(u32x2_type, extract_word(0), extract_word(1));
}

Id SharedWord(EmitContext& ctx, Id index) {
    if (ctx.uses_explicit_workgroup_layout) {
        return ctx.OpAccessChain(ctx.shared_u32, ctx.shared_memory_u32, ctx.u32_zero_value, index);
    }
    return ctx.OpAccessChain(ctx.shared_u32, ctx.shared_memory_u32, index);
}

void SubgroupBarrier(EmitContext& ctx) {
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Subgroup))};
    ctx.OpControlBarrier(scope, scope, ctx.Const(SubgroupBarrierSemantics));
}

// One HMMA.16816.F16 (or .1688 with zero upper halves) of a 32-lane warp on 16x16x16 matrices: the
// NVIDIA fragments go to row-major A, column-major B (columns 8-15 zero) and row-major C tiles in
// shared memory, one cooperative-matrix multiply-add replaces the per-lane emulation, and each lane
// reads its two D words back from where it wrote C.
Id EmitCooperativeTensorMulAdd(EmitContext& ctx, const CooperativeHmmaTypes& types,
                               u32 scratch_word, Id a, Id b, Id c) {
    const Id u32ty = ctx.U32[1];
    const auto imm = [&](u32 value) { return ctx.Const(value); };
    const auto add = [&](Id lhs, Id rhs) { return ctx.OpIAdd(u32ty, lhs, rhs); };
    const Id lane = ctx.OpBitwiseAnd(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id),
                                     imm(31));
    const Id subgroup = ctx.OpLoad(u32ty, ctx.tensor_subgroup_id);
    const Id base = add(imm(scratch_word), ctx.OpIMul(u32ty, subgroup, imm(384)));
    const Id tile_a = base;
    const Id tile_b = add(base, imm(128));
    const Id tile_c = add(base, imm(256));
    const Id g = ctx.OpShiftRightLogical(u32ty, lane, imm(2));
    const Id t = ctx.OpBitwiseAnd(u32ty, lane, imm(3));
    // Word of (row, first column pair) in a 16x16 fp16 tile with 8 words a row/column.
    const Id row_lo = add(ctx.OpShiftLeftLogical(u32ty, g, imm(3)), t);  // row g,   cols 2t
    const Id row_hi = add(row_lo, imm(64));                                // row g+8, cols 2t
    const auto store = [&](Id tile, Id word, Id value) {
        ctx.OpStore(SharedWord(ctx, add(tile, word)), value);
    };
    store(tile_a, row_lo, ctx.OpCompositeExtract(u32ty, a, 0));
    store(tile_a, row_hi, ctx.OpCompositeExtract(u32ty, a, 1));
    store(tile_a, add(row_lo, imm(4)), ctx.OpCompositeExtract(u32ty, a, 2));
    store(tile_a, add(row_hi, imm(4)), ctx.OpCompositeExtract(u32ty, a, 3));
    // B column g holds k pairs t (b0) and t+4 (b1); columns 8-15 (words 64-127) are zero.
    store(tile_b, row_lo, ctx.OpCompositeExtract(u32ty, b, 0));
    store(tile_b, add(row_lo, imm(4)), ctx.OpCompositeExtract(u32ty, b, 1));
    store(tile_b, add(lane, imm(64)), ctx.u32_zero_value);
    store(tile_b, add(lane, imm(96)), ctx.u32_zero_value);
    store(tile_c, row_lo, ctx.OpCompositeExtract(u32ty, c, 0));
    store(tile_c, row_hi, ctx.OpCompositeExtract(u32ty, c, 1));
    SubgroupBarrier(ctx);
    const Id stride = imm(8);
    // OpCooperativeMatrixLoadKHR %type %pointer %layout %stride after PatchCooperativeHmma.
    const auto load = [&](Id type, Id tile, u32 layout) {
        return ctx.OpBitFieldUExtract(type, SharedWord(ctx, tile), imm(layout), stride);
    };
    const Id matrix_a = load(types.matrix_a, tile_a, LayoutRowMajor);
    const Id matrix_b = load(types.matrix_b, tile_b, LayoutColumnMajor);
    const Id matrix_c = load(types.matrix_c, tile_c, LayoutRowMajor);
    // OpCooperativeMatrixMulAddKHR and OpCooperativeMatrixStoreKHR %pointer %object %layout
    // %stride after PatchCooperativeHmma.
    const Id matrix_d = ctx.OpSelect(types.matrix_c, matrix_a, matrix_b, matrix_c);
    const std::array<Sirit::MemoryAccessLiteral, 2> store_operands{imm(LayoutRowMajor).value,
                                                                   stride.value};
    ctx.OpStore(SharedWord(ctx, tile_c), matrix_d,
                std::span<const Sirit::MemoryAccessLiteral>(store_operands));
    SubgroupBarrier(ctx);
    const Id d0 = ctx.OpLoad(u32ty, SharedWord(ctx, add(tile_c, row_lo)));
    const Id d1 = ctx.OpLoad(u32ty, SharedWord(ctx, add(tile_c, row_hi)));
    return ctx.OpCompositeConstruct(ctx.U32[2], d0, d1);
}
} // Anonymous namespace

std::optional<u32> ReserveCooperativeHmmaScratch(const Profile& profile, IR::Program& program) {
    if (!profile.support_cooperative_hmma_16x16x16 || !SupportsCooperativeHmma(program)) {
        return std::nullopt;
    }
    bool has_hmma{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            has_hmma |= inst.GetOpcode() == IR::Opcode::TensorMulAdd;
        }
    }
    if (!has_hmma) {
        return std::nullopt;
    }
    const u32 invocations = program.workgroup_size[0] * program.workgroup_size[1] *
                            program.workgroup_size[2];
    const u32 subgroups = invocations / GUEST_WARP_LANES;
    const u32 scratch_offset = (program.shared_memory_size + 15U) & ~15U;
    const u32 total = scratch_offset + subgroups * 384U * 4U;
    // Past the device limit the pipeline would fail to build: keep the emulation.
    if (profile.max_compute_shared_memory_size != 0 &&
        total > profile.max_compute_shared_memory_size) {
        return std::nullopt;
    }
    program.shared_memory_size = total;
    return scratch_offset / 4U;
}

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    if (ctx.tensor_scratch_word) {
        ctx.cooperative_hmma =
            DefineCooperativeHmmaTypes(ctx, ctx.U32[1], 16, ctx.profile.supported_spirv);
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        const Id input_u32 = ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]);
        ctx.tensor_subgroup_id = ctx.AddGlobalVariable(input_u32, spv::StorageClass::Input);
        ctx.Decorate(ctx.tensor_subgroup_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupId);
        ctx.interfaces.push_back(ctx.tensor_subgroup_id);
        return;
    }
    const bool cooperative{ctx.profile.support_cooperative_hmma && SupportsCooperativeHmma(program)};
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            if (depth == 16 && cooperative) {
                if (!ctx.cooperative_hmma) {
                    ctx.cooperative_hmma =
                        DefineCooperativeHmmaTypes(ctx, ctx.U32[1], 8, ctx.profile.supported_spirv);
                }
                continue;
            }
            needed[depth == 16] = true;
        }
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.SupportsSubgroups() ||
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
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
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
        const Id row_base = and_(lane, 28);
        const Id col_base = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3));
        const Id a_base = add(warp_base, row_base);
        const Id b_base = add(warp_base, col_base);
        // Unrolling a K=8 group lets its shuffles overlap, which latency-bound small dispatches
        // need; unrolling all of K=16 raises register pressure and slows large dispatches.
        const auto emit_group = [&](auto pick) {
            for (u32 component = 0; component < 4; ++component) {
                const Id b = pick(inputs[4], inputs[5]);
                std::array<Id, 2> bs;
                for (u32 col = 0; col < 2; ++col)
                    bs[col] = unpack(ctx.OpGroupNonUniformShuffle(
                        u32ty, imm(3), b, add(b_base, imm(component + col * 4))));
                for (u32 row = 0; row < 2; ++row) {
                    const Id as = unpack(ctx.OpGroupNonUniformShuffle(
                        u32ty, imm(3), pick(inputs[row], inputs[row + 2]),
                        add(a_base, imm(component))));
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
            for (const Id accumulator : accumulators)
                ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        };
        if (variant == 0) {
            emit_group([](Id low, Id) { return low; });
        } else {
            const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(),
                     end = ctx.OpLabel();
            ctx.OpBranch(header);
            ctx.AddLabel(header);
            const Id group = ctx.OpLoad(u32ty, counter);
            const Id more = ctx.OpULessThan(ctx.U1, group, imm(2));
            ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
            ctx.OpBranchConditional(more, body, end);
            ctx.AddLabel(body);
            const Id upper = ctx.OpIEqual(ctx.U1, group, imm(1));
            emit_group([&](Id low, Id high) { return ctx.OpSelect(u32ty, upper, high, low); });
            ctx.OpBranch(next);
            ctx.AddLabel(next);
            ctx.OpStore(counter, add(group, imm(1)));
            ctx.OpBranch(header);
            ctx.AddLabel(end);
        }
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    if (ctx.tensor_scratch_word) {
        return EmitCooperativeTensorMulAdd(ctx, *ctx.cooperative_hmma, *ctx.tensor_scratch_word, a,
                                           b, c);
    }
    if (depth == 16 && ctx.cooperative_hmma) {
        return EmitCooperativeHmma(ctx, *ctx.cooperative_hmma, ctx.U32[1], ctx.U32[2], a, b, c);
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16], a, b, c);
}

void PatchCooperativeHmma(std::vector<u32>& code, const CooperativeHmmaTypes& types) {
    const auto is_matrix{[&](u32 id) {
        return id == types.matrix_a.value || id == types.matrix_b.value ||
               id == types.matrix_c.value;
    }};
    std::vector<u32> products;
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            return;
        }
        const auto opcode{static_cast<spv::Op>(code[offset] & 0xFFFFu)};
        std::optional<spv::Op> replacement;
        if (opcode == spv::Op::OpTypeStruct && is_matrix(code[offset + 1])) {
            replacement = spv::Op::OpTypeCooperativeMatrixKHR;
        } else if (opcode == spv::Op::OpSelect && code[offset + 1] == types.matrix_c.value) {
            replacement = spv::Op::OpCooperativeMatrixMulAddKHR;
            products.push_back(code[offset + 2]);
        } else if (opcode == spv::Op::OpBitFieldUExtract && is_matrix(code[offset + 1])) {
            replacement = spv::Op::OpCooperativeMatrixLoadKHR;
        } else if (opcode == spv::Op::OpStore && word_count == 5 &&
                   std::ranges::find(products, code[offset + 2]) != products.end()) {
            replacement = spv::Op::OpCooperativeMatrixStoreKHR;
        }
        if (replacement) {
            code[offset] = (code[offset] & 0xFFFF0000u) | static_cast<u32>(*replacement);
        }
        offset += word_count;
    }
}

std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version) {
    Sirit::Module module{spirv_version};
    module.AddCapability(spv::Capability::Shader);
    module.AddCapability(spv::Capability::GroupNonUniform);

    const Id void_type{module.TypeVoid()};
    const Id u32_type{module.TypeUInt(32)};
    const Id u32x2_type{module.TypeVector(u32_type, 2)};
    const Id u32x4_type{module.TypeVector(u32_type, 4)};
    const CooperativeHmmaTypes types{
        DefineCooperativeHmmaTypes(module, u32_type, 8, spirv_version)};

    const Id words_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(words_type, spv::Decoration::ArrayStride, 4U);
    const Id block_type{module.TypeStruct(words_type)};
    module.Decorate(block_type, spv::Decoration::Block);
    module.MemberDecorate(block_type, 0U, spv::Decoration::Offset, 0U);
    const Id buffer{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::StorageBuffer, block_type),
        spv::StorageClass::StorageBuffer)};
    module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
    module.Decorate(buffer, spv::Decoration::Binding, 0U);
    const Id lane_variable{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::Input, u32_type), spv::StorageClass::Input)};
    module.Decorate(lane_variable, spv::Decoration::BuiltIn,
                    spv::BuiltIn::SubgroupLocalInvocationId);
    const Id word_pointer{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};
    const Id zero{module.Constant(u32_type, 0U)};

    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const Id base{module.OpIMul(u32_type, module.OpLoad(u32_type, lane_variable),
                                module.Constant(u32_type, COOPERATIVE_HMMA_TEST_LANE_WORDS))};
    const auto word_at{[&](u32 index) {
        return module.OpAccessChain(word_pointer, buffer, zero,
                                    module.OpIAdd(u32_type, base,
                                                  module.Constant(u32_type, index)));
    }};
    const auto load{[&](u32 index) { return module.OpLoad(u32_type, word_at(index)); }};
    const Id a{module.OpCompositeConstruct(u32x4_type, load(0), load(1), load(2), load(3))};
    const Id b{module.OpCompositeConstruct(u32x2_type, load(4), load(5))};
    const Id c{module.OpCompositeConstruct(u32x2_type, load(6), load(7))};
    const Id d{EmitCooperativeHmma(module, types, u32_type, u32x2_type, a, b, c)};
    module.OpStore(word_at(8), module.OpCompositeExtract(u32_type, d, 0U));
    module.OpStore(word_at(9), module.OpCompositeExtract(u32_type, d, 1U));
    module.OpReturn();
    module.OpFunctionEnd();

    // Since SPIR-V 1.4 the interface lists every global the entry point uses.
    std::vector<Id> interfaces{lane_variable};
    if (spirv_version >= 0x00010400U) {
        interfaces.push_back(buffer);
    }
    module.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", interfaces);
    module.AddExecutionMode(main, spv::ExecutionMode::LocalSize, GUEST_WARP_LANES, 1U, 1U);

    std::vector<u32> code{module.Assemble()};
    PatchCooperativeHmma(code, types);
    return code;
}

} // namespace Shader::Backend::SPIRV
