// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Switch 2 (NVN) task and mesh programs. They talk to the hardware through the stage buffer
// (ISBE): a task program writes the number of mesh workgroups at byte 4 of its output region and
// its payload after it; each mesh workgroup reads that region (its own index at byte 0) and
// writes its vertices and primitives to an attribute region in groups of 32, one 32-word line per
// attribute, plus the primitive count and 8-bit vertex indices to a map region. Here the task's
// output region is the task payload, the mesh output regions live in workgroup memory, and the
// epilogue (reached by every invocation, see structured_control_flow.cpp) hands them to Vulkan.

#include <algorithm>
#include <span>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
void Barrier(EmitContext& ctx) {
    ctx.OpControlBarrier(ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                    spv::MemorySemanticsMask::WorkgroupMemory)));
}

Id LocalInvocation(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0U);
}

Id Clamped(EmitContext& ctx, Id index, u32 count) {
    return ctx.OpUMin(ctx.U32[1], index, ctx.Const(count - 1));
}

Id EmulationWord(EmitContext& ctx, Id word) {
    return ctx.OpAccessChain(ctx.mesh_emulation_u32, ctx.mesh_emulation_buffer,
                             ctx.u32_zero_value, word);
}

Id LoadEmulationWord(EmitContext& ctx, u32 word) {
    return ctx.OpLoad(ctx.U32[1], EmulationWord(ctx, ctx.Const(word)));
}

Id PayloadWord(EmitContext& ctx, Id index) {
    const Id clamped{Clamped(ctx, index, ctx.mesh_info.payload_words)};
    if (Sirit::ValidId(ctx.mesh_emulation_payload)) {
        return EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.mesh_emulation_payload, clamped));
    }
    return ctx.OpAccessChain(ctx.mesh_payload_u32, ctx.mesh_payload, clamped);
}

Id WorkgroupIndex(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U);
}

Id SharedWord(EmitContext& ctx, Id array, Id index) {
    return ctx.OpAccessChain(ctx.mesh_shared_u32, array, index);
}

// Runs body under `if (condition)`.
template <typename Func>
void If(EmitContext& ctx, Id condition, Func&& body) {
    const Id then_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, then_label, merge_label);
    ctx.AddLabel(then_label);
    body();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

// Emulated programs: the storage buffer too.
void EmulationBarrier(EmitContext& ctx) {
    ctx.OpControlBarrier(ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                    spv::MemorySemanticsMask::WorkgroupMemory |
                                                    spv::MemorySemanticsMask::UniformMemory)));
}

// Runs body(counter) for counter = first; counter < end; counter += step.
template <typename Func>
void Loop(EmitContext& ctx, Id first, Id end, u32 step, Func&& body) {
    ctx.OpStore(ctx.mesh_emulation_counter, first);
    const Id header_label{ctx.OpLabel()};
    const Id check_label{ctx.OpLabel()};
    const Id body_label{ctx.OpLabel()};
    const Id continue_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpBranch(header_label);
    ctx.AddLabel(header_label);
    ctx.OpLoopMerge(merge_label, continue_label, spv::LoopControlMask::MaskNone);
    ctx.OpBranch(check_label);
    ctx.AddLabel(check_label);
    const Id counter{ctx.OpLoad(ctx.U32[1], ctx.mesh_emulation_counter)};
    ctx.OpBranchConditional(ctx.OpULessThan(ctx.U1, counter, end), body_label, merge_label);
    ctx.AddLabel(body_label);
    body(counter);
    ctx.OpBranch(continue_label);
    ctx.AddLabel(continue_label);
    ctx.OpStore(ctx.mesh_emulation_counter, ctx.OpIAdd(ctx.U32[1], counter, ctx.Const(step)));
    ctx.OpBranch(header_label);
    ctx.AddLabel(merge_label);
}

// Copies the lines of element `index` (a vertex or a primitive) from the attribute region.
void CopyLines(EmitContext& ctx, Id index, u32 base, u32 num_lines,
               const std::vector<EmitContext::MeshLineTarget>& targets) {
    // Word of line 0: base + (index / 32) * num_lines * 32 + index % 32.
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], index, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1],
                              ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              ctx.OpIAdd(ctx.U32[1], slot, ctx.Const(base)))};
    for (u32 line = 0; line < num_lines; ++line) {
        const EmitContext::MeshLineTarget& target{targets[line]};
        if (!Sirit::ValidId(target.variable)) {
            continue;
        }
        const Id word{ctx.OpLoad(
            ctx.U32[1],
            SharedWord(ctx, ctx.mesh_attributes, ctx.OpIAdd(ctx.U32[1], first, ctx.Const(line * 32))))};
        const Id value{target.is_float ? ctx.OpBitcast(ctx.F32[1], word) : word};
        const Id pointer_type{target.is_float ? ctx.output_f32 : ctx.output_u32};
        const Id pointer{target.element == ~0U
                             ? ctx.OpAccessChain(pointer_type, target.variable, index)
                             : ctx.OpAccessChain(pointer_type, target.variable, index,
                                                 ctx.Const(target.element))};
        ctx.OpStore(pointer, value);
    }
}

// OpenGL-style clip space: z = (z + w) / 2, from the stage buffer's position lines.
void ConvertDepth(EmitContext& ctx, Id vertex) {
    const auto& lines{ctx.mesh_info.vertex_lines};
    const auto z_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionZ))};
    const auto w_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionW))};
    if (z_line == lines.end() || w_line == lines.end()) {
        return;
    }
    const u32 num_lines{static_cast<u32>(lines.size())};
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], vertex, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], vertex, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              slot)};
    const auto read = [&](auto line) {
        const u32 offset{static_cast<u32>(line - lines.begin()) * 32};
        return ctx.OpBitcast(ctx.F32[1],
                             ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_attributes,
                                                               ctx.OpIAdd(ctx.U32[1], first,
                                                                          ctx.Const(offset)))));
    };
    const Id z{ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], read(z_line), read(w_line)),
                          ctx.Const(0.5f))};
    ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, ctx.output_position, vertex, ctx.Const(2U)), z);
}

// Emulated task program: launches its mesh workgroups by taking output slots for them (capacity
// permitting), naming its workgroup and their index in each, and copies its payload to the
// storage buffer. The slots' high mark is the mesh dispatch.
void TaskEmulationEpilogue(EmitContext& ctx) {
    EmulationBarrier(ctx);
    const Id invocation{LocalInvocation(ctx)};
    const Id task{WorkgroupIndex(ctx)};
    const Id capacity{LoadEmulationWord(ctx, MESH_EMULATION_CAPACITY_WORD)};
    const Id count{
        ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(1U))), capacity)};
    If(ctx, ctx.OpIEqual(ctx.U1, invocation, ctx.u32_zero_value), [&] {
        const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Device))};
        const Id semantics{ctx.u32_zero_value};
        const Id base{ctx.OpAtomicIAdd(
            ctx.U32[1], EmulationWord(ctx, ctx.Const(MESH_EMULATION_ALLOCATED_WORD)), scope,
            semantics, count)};
        const Id room{ctx.OpSelect(ctx.U32[1], ctx.OpULessThan(ctx.U1, base, capacity),
                                   ctx.OpISub(ctx.U32[1], capacity, base), ctx.u32_zero_value)};
        const Id launched{ctx.OpUMin(ctx.U32[1], count, room)};
        // Past the slots (base beyond them) the high mark stays at the capacity.
        const Id end{ctx.OpUMin(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], base, launched), capacity)};
        ctx.OpAtomicUMax(ctx.U32[1], EmulationWord(ctx, ctx.Const(MESH_EMULATION_DISPATCH_WORD)),
                         scope, semantics, end);
        ctx.OpStore(ctx.mesh_emulation_shared_base, base);
        ctx.OpStore(ctx.mesh_emulation_shared_count, launched);
    });
    EmulationBarrier(ctx);
    const Id base{ctx.OpLoad(ctx.U32[1], ctx.mesh_emulation_shared_base)};
    const Id launched{ctx.OpLoad(ctx.U32[1], ctx.mesh_emulation_shared_count)};
    const Id task_map{LoadEmulationWord(ctx, MESH_EMULATION_TASK_MAP_WORD)};
    Loop(ctx, invocation, launched, ctx.mesh_threads, [&](Id index) {
        const Id slot{ctx.OpIAdd(ctx.U32[1], base, index)};
        const Id word{ctx.OpIAdd(ctx.U32[1], task_map,
                                 ctx.OpShiftLeftLogical(ctx.U32[1], slot, ctx.Const(1U)))};
        ctx.OpStore(EmulationWord(ctx, word), task);
        ctx.OpStore(EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], word, ctx.Const(1U))), index);
    });
    const u32 payload_words{ctx.mesh_info.payload_words};
    const Id payload{ctx.OpIAdd(ctx.U32[1], LoadEmulationWord(ctx, MESH_EMULATION_PAYLOAD_WORD),
                                ctx.OpIMul(ctx.U32[1], task, ctx.Const(payload_words)))};
    const u32 threads{ctx.mesh_threads};
    for (u32 first = 0; first < payload_words; first += threads) {
        const Id word{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            ctx.OpStore(EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], payload, word)),
                        ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, word)));
        };
        if (first + threads > payload_words) {
            If(ctx, ctx.OpULessThan(ctx.U1, word, ctx.Const(payload_words)), copy);
        } else {
            copy();
        }
    }
}

// Emulated mesh program: its regions are in the storage buffer already. The first invocation
// sizes the vertex shader's draw: max_primitives triangles per slot.
void MeshEmulationEpilogue(EmitContext& ctx) {
    const Id first{ctx.OpLogicalAnd(
        ctx.U1, ctx.OpIEqual(ctx.U1, WorkgroupIndex(ctx), ctx.u32_zero_value),
        ctx.OpIEqual(ctx.U1, LocalInvocation(ctx), ctx.u32_zero_value))};
    If(ctx, first, [&] {
        const Id slots{ctx.OpUMin(
            ctx.U32[1],
            ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.num_workgroups), 0U),
            LoadEmulationWord(ctx, MESH_EMULATION_CAPACITY_WORD))};
        ctx.OpStore(EmulationWord(ctx, ctx.Const(MESH_EMULATION_DRAW_WORD)),
                    ctx.OpIMul(ctx.U32[1], slots, ctx.Const(ctx.mesh_info.max_primitives * 3)));
    });
}

// The slot's primitive count, after its map bytes.
Id PrimitiveCountWord(EmitContext& ctx) {
    return EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.mesh_emulation_map,
                                         ctx.Const(ctx.mesh_map_bytes)));
}

// Emulated mesh program: its workgroup is an output slot; after a task program the task map
// names the task workgroup (whose payload it reads) and its index in that launch. In the entry
// block: no branches.
void MeshEmulationPrologue(EmitContext& ctx) {
    const Id slot{WorkgroupIndex(ctx)};
    if (ctx.runtime_info.mesh_emulation_task) {
        const Id pair{ctx.OpIAdd(ctx.U32[1], LoadEmulationWord(ctx, MESH_EMULATION_TASK_MAP_WORD),
                                 ctx.OpShiftLeftLogical(ctx.U32[1], slot, ctx.Const(1U)))};
        const Id task{ctx.OpLoad(ctx.U32[1], EmulationWord(ctx, pair))};
        ctx.mesh_emulation_index = ctx.OpLoad(
            ctx.U32[1], EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], pair, ctx.Const(1U))));
        ctx.mesh_emulation_payload =
            ctx.OpIAdd(ctx.U32[1], LoadEmulationWord(ctx, MESH_EMULATION_PAYLOAD_WORD),
                       ctx.OpIMul(ctx.U32[1], task, ctx.Const(ctx.mesh_info.payload_words)));
    } else {
        ctx.mesh_emulation_index = slot;
    }
    ctx.mesh_emulation_attributes =
        ctx.OpIAdd(ctx.U32[1], LoadEmulationWord(ctx, MESH_EMULATION_ATTRIBUTE_WORD),
                   ctx.OpIMul(ctx.U32[1], slot, ctx.Const(ctx.mesh_attribute_words)));
    ctx.mesh_emulation_map =
        ctx.OpIAdd(ctx.U32[1], LoadEmulationWord(ctx, MESH_EMULATION_MAP_WORD),
                   ctx.OpIMul(ctx.U32[1], slot, ctx.Const(MeshEmulationMapWords(ctx.mesh_info))));
    // The slot's map region starts zeroed: map bytes the program leaves unwritten name vertex 0,
    // not another draw's vertices. Every invocation clears a share of it (past the end, the
    // last word again: no branch in the entry block).
    const u32 map_words{MeshEmulationMapWords(ctx.mesh_info)};
    const u32 threads{ctx.mesh_threads};
    const Id invocation{LocalInvocation(ctx)};
    for (u32 first = 0; first < map_words; first += threads) {
        const Id word{ctx.OpUMin(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first)),
                                 ctx.Const(map_words - 1))};
        ctx.OpStore(EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.mesh_emulation_map, word)),
                    ctx.u32_zero_value);
    }
    EmulationBarrier(ctx);
}

void TaskEpilogue(EmitContext& ctx) {
    Barrier(ctx);
    const Id count{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(1U)))};
    const Id one{ctx.Const(1U)};
    // Marker for OpEmitMeshTasksEXT count, 1, 1, payload (PatchMeshInstructions).
    ctx.OpStore(count, one);
    ctx.OpStore(one, ctx.mesh_payload);
    ctx.mesh_tasks_marker = {count, one, one, ctx.mesh_payload};
}

void MeshEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    Barrier(ctx);
    const Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.mesh_primitive_count),
                              ctx.Const(mesh.max_primitives))};
    const Id max_vertices{ctx.Const(mesh.max_vertices)};
    // Marker for OpSetMeshOutputsEXT max_vertices, count (PatchMeshInstructions).
    ctx.OpStore(max_vertices, count);
    ctx.mesh_outputs_marker = {max_vertices, count};

    const Id invocation{LocalInvocation(ctx)};
    const u32 threads{ctx.mesh_threads};
    const bool convert_depth{ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc &&
                             Sirit::ValidId(ctx.output_position)};
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    for (u32 first = 0; first < mesh.max_vertices; first += threads) {
        const Id vertex{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            CopyLines(ctx, vertex, 0, vertex_lines, ctx.mesh_vertex_targets);
            if (convert_depth) {
                ConvertDepth(ctx, vertex);
            }
        };
        if (first + threads > mesh.max_vertices) {
            If(ctx, ctx.OpULessThan(ctx.U1, vertex, max_vertices), copy);
        } else {
            copy();
        }
    }
    const u32 primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
    for (u32 first = 0; first < mesh.max_primitives; first += threads) {
        const Id primitive{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        If(ctx, ctx.OpULessThan(ctx.U1, primitive, count), [&] {
            // Vertex indices: map bytes 4 + 3 * primitive.
            const Id byte{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(3U)),
                                     ctx.Const(4U))};
            std::array<Id, 3> indices;
            for (u32 element = 0; element < 3; ++element) {
                const Id address{ctx.OpIAdd(ctx.U32[1], byte, ctx.Const(element))};
                indices[element] = ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, address));
            }
            const Id output_indices{ctx.TypePointer(spv::StorageClass::Output, ctx.U32[3])};
            ctx.OpStore(ctx.OpAccessChain(output_indices, ctx.mesh_primitive_indices, primitive),
                        ctx.OpCompositeConstruct(ctx.U32[3], std::span<const Id>(indices)));
            CopyLines(ctx, primitive, ctx.mesh_primitive_base, primitive_lines,
                      ctx.mesh_primitive_targets);
        });
    }
}
} // Anonymous namespace

Id EmitIsbeRead(EmitContext& ctx, Id address) {
    // An emulated mesh workgroup is an output slot: its index in the launch is the prologue's.
    Id workgroup{Sirit::ValidId(ctx.mesh_emulation_index) ? ctx.mesh_emulation_index
                                                           : WorkgroupIndex(ctx)};
    if (Sirit::ValidId(ctx.mesh_draw_base_push_constant)) {
        const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1])};
        const Id pointer{ctx.OpAccessChain(pointer_type, ctx.mesh_draw_base_push_constant,
                                           ctx.Const(ctx.mesh_draw_base_member_index))};
        workgroup = ctx.OpIAdd(ctx.U32[1], workgroup, ctx.OpLoad(ctx.U32[1], pointer));
    }
    if (ctx.stage == Stage::Task) {
        return workgroup; // the input region holds the workgroup index (VERTEX_ID)
    }
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    const Id word{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, index))};
    return ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, address, ctx.u32_zero_value), workgroup,
                        word);
}

void EmitIsbeWriteAttribute(EmitContext& ctx, Id address, Id value) {
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    if (ctx.stage == Stage::Task) {
        ctx.OpStore(PayloadWord(ctx, index), value);
        return;
    }
    const Id clamped{Clamped(ctx, index, ctx.mesh_attribute_words)};
    if (ctx.mesh_emulated) {
        ctx.OpStore(
            EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.mesh_emulation_attributes, clamped)),
            value);
        return;
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_attributes, clamped), value);
}

void EmitIsbeWriteMapU8(EmitContext& ctx, Id address, Id value) {
    if (ctx.stage != Stage::Mesh) {
        throw NotImplementedException("Stage buffer map write outside mesh programs");
    }
    const Id clamped{Clamped(ctx, address, ctx.mesh_map_bytes)};
    const Id byte{ctx.OpBitwiseAnd(ctx.U32[1], value, ctx.Const(0xffU))};
    if (ctx.mesh_emulated) {
        ctx.OpStore(EmulationWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.mesh_emulation_map, clamped)),
                    byte);
        return;
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_map, clamped), byte);
}

void EmitIsbeWriteMapU32(EmitContext& ctx, const IR::Value& address, Id value) {
    // The primitive count: the map is written from its end, so its first word is at byte 3.
    if (ctx.stage != Stage::Mesh || !address.IsImmediate() || address.U32() != 3) {
        throw NotImplementedException("32-bit stage buffer map write");
    }
    if (ctx.mesh_emulated) {
        ctx.OpStore(PrimitiveCountWord(ctx), value);
        return;
    }
    ctx.OpStore(ctx.mesh_primitive_count, value);
}

void EmitMeshPrologue(EmitContext& ctx) {
    if (ctx.stage != Stage::Mesh) {
        return;
    }
    if (ctx.mesh_emulated) {
        MeshEmulationPrologue(ctx);
        return;
    }
    // Every invocation stores the zero: no branch here, the entry block's label is a phi
    // predecessor for later blocks.
    ctx.OpStore(ctx.mesh_primitive_count, ctx.u32_zero_value);
    Barrier(ctx);
}

void EmitMeshEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Task) {
        ctx.mesh_emulated ? TaskEmulationEpilogue(ctx) : TaskEpilogue(ctx);
    } else if (ctx.stage == Stage::Mesh) {
        ctx.mesh_emulated ? MeshEmulationEpilogue(ctx) : MeshEpilogue(ctx);
    }
}

} // namespace Shader::Backend::SPIRV
