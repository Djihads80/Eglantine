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

Id PayloadWord(EmitContext& ctx, Id index) {
    return ctx.OpAccessChain(ctx.mesh_payload_u32, ctx.mesh_payload,
                             Clamped(ctx, index, ctx.mesh_info.payload_words));
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

Id BufferWord(EmitContext& ctx, Id buffer, Id index) {
    return ctx.OpAccessChain(ctx.mesh_output_u32, buffer, ctx.u32_zero_value, index);
}

Id WorkgroupX(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U);
}

// Mesh emulation: the task workgroup's payload (its mesh workgroup count at word 1) goes to its
// block of the task payload buffer, for the distribution pass and the mesh workgroups.
void ComputeTaskEpilogue(EmitContext& ctx) {
    Barrier(ctx);
    const u32 stride{ctx.runtime_info.mesh_task_payload_stride};
    const u32 words{(std::min)(ctx.mesh_info.payload_words, stride)};
    const Id block{ctx.OpIMul(ctx.U32[1], WorkgroupX(ctx), ctx.Const(stride))};
    const Id invocation{LocalInvocation(ctx)};
    const u32 threads{(std::max)(ctx.mesh_threads, 1U)};
    for (u32 first = 0; first < words; first += threads) {
        const Id word{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            ctx.OpStore(BufferWord(ctx, ctx.mesh_task_payload_buffer,
                                   ctx.OpIAdd(ctx.U32[1], block, word)),
                        ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, word)));
        };
        if (first + threads > words) {
            If(ctx, ctx.OpULessThan(ctx.U1, word, ctx.Const(words)), copy);
        } else {
            copy();
        }
    }
}

void TaskEpilogue(EmitContext& ctx) {
    if (ctx.runtime_info.mesh_as_compute) {
        ComputeTaskEpilogue(ctx);
        return;
    }
    Barrier(ctx);
    const Id count{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(1U)))};
    const Id one{ctx.Const(1U)};
    // Marker for OpEmitMeshTasksEXT count, 1, 1, payload (PatchMeshInstructions).
    ctx.OpStore(count, one);
    ctx.OpStore(one, ctx.mesh_payload);
    ctx.mesh_tasks_marker = {count, one, one, ctx.mesh_payload};
}

Id OutputWord(EmitContext& ctx, Id index) {
    return ctx.OpAccessChain(ctx.mesh_output_u32, ctx.mesh_output_buffer, ctx.u32_zero_value,
                             index);
}

// Mesh emulation (RuntimeInfo::mesh_as_compute): writes this workgroup's block of the output
// buffer, laid out as MeshEmulationBlockWords describes, instead of the mesh outputs.
void ComputeMeshEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    Barrier(ctx);
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    const u32 vertex_words{mesh.max_vertices * vertex_lines};
    const Id workgroup{
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U)};
    Id block{ctx.OpIMul(ctx.U32[1], workgroup, ctx.Const(MeshEmulationBlockWords(mesh)))};
    if (Sirit::ValidId(ctx.mesh_task_args_buffer)) {
        // With a task program the draw's blocks start where the distribution pass put them.
        block = ctx.OpIAdd(ctx.U32[1], block,
                           ctx.OpLoad(ctx.U32[1], BufferWord(ctx, ctx.mesh_task_args_buffer,
                                                             ctx.Const(MESH_EMULATION_ARGS_BASE))));
    }
    const Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.mesh_primitive_count),
                              ctx.Const(mesh.max_primitives))};
    const Id invocation{LocalInvocation(ctx)};
    const u32 threads{ctx.mesh_threads};

    // Vertex v, line l: output word v * vertex_lines + l, from the stage buffer's line groups.
    for (u32 first = 0; first < mesh.max_vertices; first += threads) {
        const Id vertex{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            const Id group{ctx.OpShiftRightLogical(ctx.U32[1], vertex, ctx.Const(5U))};
            const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], vertex, ctx.Const(31U))};
            const Id source{ctx.OpIAdd(
                ctx.U32[1], ctx.OpIMul(ctx.U32[1], group, ctx.Const(vertex_lines * 32)), slot)};
            const Id destination{ctx.OpIAdd(
                ctx.U32[1], block, ctx.OpIMul(ctx.U32[1], vertex, ctx.Const(vertex_lines)))};
            for (u32 line = 0; line < vertex_lines; ++line) {
                const Id word{ctx.OpLoad(
                    ctx.U32[1],
                    SharedWord(ctx, ctx.mesh_attributes,
                               ctx.OpIAdd(ctx.U32[1], source, ctx.Const(line * 32))))};
                ctx.OpStore(OutputWord(ctx, ctx.OpIAdd(ctx.U32[1], destination, ctx.Const(line))),
                            word);
            }
        };
        if (first + threads > mesh.max_vertices) {
            If(ctx, ctx.OpULessThan(ctx.U1, vertex, ctx.Const(mesh.max_vertices)), copy);
        } else {
            copy();
        }
    }
    // Primitive p: its three vertex indices, or 0 0 0 (a degenerate triangle) past the count.
    const Id index_base{ctx.OpIAdd(ctx.U32[1], block, ctx.Const(vertex_words))};
    for (u32 first = 0; first < mesh.max_primitives; first += threads) {
        const Id primitive{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto write = [&] {
            const Id used{ctx.OpULessThan(ctx.U1, primitive, count)};
            const Id byte{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(3U)),
                                     ctx.Const(4U))};
            const Id destination{ctx.OpIAdd(
                ctx.U32[1], index_base, ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(3U)))};
            for (u32 element = 0; element < 3; ++element) {
                const Id map_index{
                    Clamped(ctx, ctx.OpIAdd(ctx.U32[1], byte, ctx.Const(element)),
                            ctx.mesh_map_bytes)};
                const Id value{ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, map_index))};
                ctx.OpStore(
                    OutputWord(ctx, ctx.OpIAdd(ctx.U32[1], destination, ctx.Const(element))),
                    ctx.OpSelect(ctx.U32[1], used, value, ctx.u32_zero_value));
            }
        };
        if (first + threads > mesh.max_primitives) {
            If(ctx, ctx.OpULessThan(ctx.U1, primitive, ctx.Const(mesh.max_primitives)), write);
        } else {
            write();
        }
    }
}

void MeshEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    if (ctx.runtime_info.mesh_as_compute) {
        ComputeMeshEpilogue(ctx);
        return;
    }
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
    // Points need a size per vertex: the fixed state one, unless the program writes its own.
    const bool points{mesh.PrimitiveVertices() == 1};
    const Id point_size{ctx.Const(ctx.runtime_info.fixed_state_point_size.value_or(1.0f))};
    for (u32 first = 0; first < mesh.max_vertices; first += threads) {
        const Id vertex{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            if (points) {
                ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, ctx.mesh_point_size, vertex),
                            point_size);
            }
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
    const u32 primitive_vertices{mesh.PrimitiveVertices()};
    for (u32 first = 0; first < mesh.max_primitives; first += threads) {
        const Id primitive{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        If(ctx, ctx.OpULessThan(ctx.U1, primitive, count), [&] {
            // Vertex indices: map bytes 4 + primitive_vertices * primitive.
            const Id byte{ctx.OpIAdd(
                ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(primitive_vertices)),
                ctx.Const(4U))};
            std::array<Id, 3> indices;
            for (u32 element = 0; element < primitive_vertices; ++element) {
                const Id address{ctx.OpIAdd(ctx.U32[1], byte, ctx.Const(element))};
                indices[element] = ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, address));
            }
            const Id index_type{ctx.U32[primitive_vertices]};
            const Id output_indices{ctx.TypePointer(spv::StorageClass::Output, index_type)};
            ctx.OpStore(ctx.OpAccessChain(output_indices, ctx.mesh_primitive_indices, primitive),
                        primitive_vertices == 1
                            ? indices[0]
                            : ctx.OpCompositeConstruct(
                                  index_type,
                                  std::span<const Id>(indices.data(), primitive_vertices)));
            CopyLines(ctx, primitive, ctx.mesh_primitive_base, primitive_lines,
                      ctx.mesh_primitive_targets);
        });
    }
}
} // Anonymous namespace

Id EmitIsbeRead(EmitContext& ctx, Id address) {
    const Id workgroup{
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U)};
    if (ctx.stage == Stage::Task) {
        return workgroup; // the input region holds the workgroup index (VERTEX_ID)
    }
    // Mesh emulation with a task: a mesh workgroup's index is within its task's, not global.
    const Id own_index{Sirit::ValidId(ctx.mesh_group_index)
                           ? ctx.OpLoad(ctx.U32[1], ctx.mesh_group_index)
                           : workgroup};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    const Id word{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, index))};
    return ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, address, ctx.u32_zero_value), own_index,
                        word);
}

void EmitIsbeWriteAttribute(EmitContext& ctx, Id address, Id value) {
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    if (ctx.stage == Stage::Task) {
        ctx.OpStore(PayloadWord(ctx, index), value);
        return;
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_attributes, Clamped(ctx, index, ctx.mesh_attribute_words)),
                value);
}

void EmitIsbeWriteMapU8(EmitContext& ctx, Id address, Id value) {
    if (ctx.stage != Stage::Mesh) {
        throw NotImplementedException("Stage buffer map write outside mesh programs");
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_map, Clamped(ctx, address, ctx.mesh_map_bytes)),
                ctx.OpBitwiseAnd(ctx.U32[1], value, ctx.Const(0xffU)));
}

void EmitIsbeWriteMapU32(EmitContext& ctx, const IR::Value& address, Id value) {
    // The primitive count: the map is written from its end, so its first word is at byte 3.
    if (ctx.stage != Stage::Mesh || !address.IsImmediate() || address.U32() != 3) {
        throw NotImplementedException("32-bit stage buffer map write");
    }
    ctx.OpStore(ctx.mesh_primitive_count, value);
}

void EmitMeshPrologue(EmitContext& ctx) {
    // No branches here: the entry block's label is a phi predecessor for later blocks.
    const bool as_compute{ctx.runtime_info.mesh_as_compute};
    if (ctx.stage == Stage::Task) {
        if (!as_compute) {
            // A task that culls all its threads never writes the mesh workgroup count, and the
            // payload starts undefined. Intel's garbage there launches enough workgroups to lose
            // the device.
            ctx.OpStore(PayloadWord(ctx, ctx.Const(1U)), ctx.u32_zero_value);
            Barrier(ctx);
            return;
        }
    } else if (ctx.stage != Stage::Mesh) {
        return;
    } else {
        // Every invocation stores the zero.
        ctx.OpStore(ctx.mesh_primitive_count, ctx.u32_zero_value);
    }
    if (as_compute) {
        const Id invocation{LocalInvocation(ctx)};
        const u32 step{(std::max)(ctx.mesh_threads, 1U)};
        const u32 words{ctx.mesh_info.payload_words};
        if (ctx.stage == Stage::Mesh && Sirit::ValidId(ctx.mesh_group_index)) {
            // Mesh emulation with a task: find this workgroup's task and its index within the
            // task's mesh workgroups, and copy the task's payload into the input region. Words
            // past the payload are clamped to its last one, so every store writes a real word.
            // The task is the last one whose first mesh workgroup is not past this one: a
            // branchless binary search of the offsets (tasks with no mesh workgroups share the
            // next task's offset and lose to it), unrolled for up to 2^17 tasks.
            const Id global{WorkgroupX(ctx)};
            const Id tasks{ctx.OpLoad(ctx.U32[1], BufferWord(ctx, ctx.mesh_task_args_buffer,
                                                             ctx.Const(MESH_EMULATION_ARGS_TASKS)))};
            const Id last_task{ctx.OpISub(ctx.U32[1], ctx.OpUMax(ctx.U32[1], tasks, ctx.Const(1U)),
                                          ctx.Const(1U))};
            Id task{ctx.u32_zero_value};
            Id remaining{tasks};
            for (u32 step_index = 0; step_index < 17; ++step_index) {
                const Id half{ctx.OpShiftRightLogical(ctx.U32[1], remaining, ctx.Const(1U))};
                const Id middle{ctx.OpUMin(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], task, half), last_task)};
                const Id first{ctx.OpLoad(ctx.U32[1],
                                          BufferWord(ctx, ctx.mesh_task_offsets_buffer, middle))};
                task = ctx.OpSelect(ctx.U32[1], ctx.OpULessThanEqual(ctx.U1, first, global), middle,
                                    task);
                remaining = ctx.OpISub(ctx.U32[1], remaining, half);
            }
            const Id first_group{
                ctx.OpLoad(ctx.U32[1], BufferWord(ctx, ctx.mesh_task_offsets_buffer, task))};
            ctx.OpStore(ctx.mesh_group_index, ctx.OpISub(ctx.U32[1], global, first_group));
            // The task wrote stride words; words past them (a larger mesh region) read zero.
            const u32 stride{(std::max)(ctx.runtime_info.mesh_task_payload_stride, 1U)};
            const Id block{ctx.OpIMul(ctx.U32[1], task, ctx.Const(stride))};
            for (u32 first = 0; first < words; first += step) {
                const Id word{ctx.OpUMin(ctx.U32[1],
                                         ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first)),
                                         ctx.Const(words - 1))};
                const Id source{ctx.OpUMin(ctx.U32[1], word, ctx.Const(stride - 1))};
                const Id value{ctx.OpLoad(ctx.U32[1],
                                          BufferWord(ctx, ctx.mesh_task_payload_buffer,
                                                     ctx.OpIAdd(ctx.U32[1], block, source)))};
                ctx.OpStore(PayloadWord(ctx, word),
                            ctx.OpSelect(ctx.U32[1],
                                         ctx.OpULessThan(ctx.U1, word, ctx.Const(stride)), value,
                                         ctx.u32_zero_value));
            }
        } else {
            // Without a task (and for the task's own output): plain workgroup memory, zeroed so
            // words nobody writes read as zero, not garbage. Indices past the region are clamped
            // to its last word, which is zeroed as well.
            for (u32 first = 0; first < words; first += step) {
                ctx.OpStore(PayloadWord(ctx, ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))),
                            ctx.u32_zero_value);
            }
        }
        if (ctx.stage == Stage::Mesh) {
            // Workgroup memory keeps whatever the previous workgroup left. A primitive that uses
            // a vertex the program never wrote would draw a stray triangle, so every vertex
            // position starts as NaN, which the rasterizer drops.
            const auto& lines{ctx.mesh_info.vertex_lines};
            const u32 num_lines{static_cast<u32>(lines.size())};
            const Id nan{ctx.Const(0x7fc00000U)};
            const u32 max_vertices{ctx.mesh_info.max_vertices};
            for (u32 line = 0; line < num_lines; ++line) {
                const auto attribute{static_cast<IR::Attribute>(lines[line])};
                if (attribute < IR::Attribute::PositionX || attribute > IR::Attribute::PositionW) {
                    continue;
                }
                for (u32 first = 0; first < max_vertices; first += step) {
                    // Past the last vertex the index is clamped to it, which is a NaN store too.
                    const Id vertex{ctx.OpUMin(ctx.U32[1],
                                               ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first)),
                                               ctx.Const(max_vertices - 1))};
                    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], vertex, ctx.Const(5U))};
                    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], vertex, ctx.Const(31U))};
                    const Id index{ctx.OpIAdd(
                        ctx.U32[1],
                        ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                        ctx.OpIAdd(ctx.U32[1], slot, ctx.Const(line * 32)))};
                    ctx.OpStore(SharedWord(ctx, ctx.mesh_attributes, index), nan);
                }
            }
        }
    }
    Barrier(ctx);
}

void EmitMeshEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Task) {
        TaskEpilogue(ctx);
    } else if (ctx.stage == Stage::Mesh) {
        MeshEpilogue(ctx);
    }
}

std::vector<u32> EmitMeshEmulationVertex(const Profile& profile, const MeshInfo& mesh, u32 binding,
                                         u32 args_binding, bool convert_depth) {
    Sirit::Module module{profile.supported_spirv};
    module.AddCapability(spv::Capability::Shader);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    module.AddExtension("SPV_KHR_storage_buffer_storage_class");

    const Id void_type{module.TypeVoid()};
    const Id u32_type{module.TypeInt(32, false)};
    const Id s32_type{module.TypeInt(32, true)};
    const Id f32_type{module.TypeFloat(32)};
    const Id vec4_type{module.TypeVector(f32_type, 4)};
    const auto constant = [&](u32 value) { return module.Constant(u32_type, value); };
    const Id zero_f{module.Constant(f32_type, 0.0f)};
    const Id one_f{module.Constant(f32_type, 1.0f)};
    const std::array<Id, 4> default_components{zero_f, zero_f, zero_f, one_f};
    const Id default_vec4{module.ConstantComposite(vec4_type, default_components)};

    // The compute pass's output buffer.
    const Id array_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
    const Id struct_type{module.TypeStruct(array_type)};
    module.Decorate(struct_type, spv::Decoration::Block);
    module.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
    module.MemberDecorate(struct_type, 0, spv::Decoration::NonWritable);
    const Id buffer{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::StorageBuffer, struct_type),
        spv::StorageClass::StorageBuffer)};
    module.Decorate(buffer, spv::Decoration::Binding, binding);
    module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
    module.Name(buffer, "mesh_emulation_output");
    const Id buffer_u32{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};
    Id args{};
    if (args_binding != ~0U) {
        args = module.AddGlobalVariable(
            module.TypePointer(spv::StorageClass::StorageBuffer, struct_type),
            spv::StorageClass::StorageBuffer);
        module.Decorate(args, spv::Decoration::Binding, args_binding);
        module.Decorate(args, spv::Decoration::DescriptorSet, 0U);
        module.Name(args, "mesh_emulation_task_args");
    }

    std::vector<Id> interfaces;
    const Id vertex_index{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::Input, s32_type), spv::StorageClass::Input)};
    module.Decorate(vertex_index, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    interfaces.push_back(vertex_index);
    const Id output_vec4{module.TypePointer(spv::StorageClass::Output, vec4_type)};
    const Id output_f32{module.TypePointer(spv::StorageClass::Output, f32_type)};
    const Id position{module.AddGlobalVariable(output_vec4, spv::StorageClass::Output)};
    module.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    interfaces.push_back(position);
    std::array<Id, IR::NUM_GENERICS> generics{};
    for (const u32 line : mesh.vertex_lines) {
        const IR::Attribute attribute{line};
        if (!IR::IsGeneric(attribute)) {
            continue;
        }
        const u32 index{IR::GenericAttributeIndex(attribute)};
        if (!Sirit::ValidId(generics[index])) {
            generics[index] = module.AddGlobalVariable(output_vec4, spv::StorageClass::Output);
            module.Decorate(generics[index], spv::Decoration::Location, index);
            interfaces.push_back(generics[index]);
        }
    }
    if (profile.supported_spirv >= 0x00010400) {
        interfaces.push_back(buffer);
        if (Sirit::ValidId(args)) {
            interfaces.push_back(args);
        }
    }

    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const auto load = [&](Id word_index) {
        return module.OpLoad(u32_type, module.OpAccessChain(buffer_u32, buffer, constant(0),
                                                             word_index));
    };
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    const u32 max_primitives{(std::max)(mesh.max_primitives, 1U)};
    // Vertex i of the draw: corner i % 3 of primitive slot i / 3, which belongs to workgroup
    // slot / max_primitives.
    const Id draw_vertex{module.OpBitcast(u32_type, module.OpLoad(s32_type, vertex_index))};
    const Id slot{module.OpUDiv(u32_type, draw_vertex, constant(3))};
    const Id corner{module.OpUMod(u32_type, draw_vertex, constant(3))};
    const Id workgroup{module.OpUDiv(u32_type, slot, constant(max_primitives))};
    const Id primitive{module.OpUMod(u32_type, slot, constant(max_primitives))};
    Id block{module.OpIMul(u32_type, workgroup, constant(MeshEmulationBlockWords(mesh)))};
    if (Sirit::ValidId(args)) {
        block = module.OpIAdd(
            u32_type, block,
            module.OpLoad(u32_type, module.OpAccessChain(buffer_u32, args, constant(0),
                                                         constant(MESH_EMULATION_ARGS_BASE))));
    }
    const Id index_word{module.OpIAdd(
        u32_type, module.OpIAdd(u32_type, block, constant(mesh.max_vertices * vertex_lines)),
        module.OpIAdd(u32_type, module.OpIMul(u32_type, primitive, constant(3)), corner))};
    const Id vertex{module.OpUMin(u32_type, load(index_word),
                                  constant((std::max)(mesh.max_vertices, 1U) - 1))};
    const Id vertex_base{
        module.OpIAdd(u32_type, block, module.OpIMul(u32_type, vertex, constant(vertex_lines)))};

    module.OpStore(position, default_vec4);
    for (const Id generic : generics) {
        if (Sirit::ValidId(generic)) {
            module.OpStore(generic, default_vec4);
        }
    }
    Id position_z{}, position_w{};
    for (u32 line = 0; line < vertex_lines; ++line) {
        const IR::Attribute attribute{mesh.vertex_lines[line]};
        const Id value{module.OpBitcast(
            f32_type, load(module.OpIAdd(u32_type, vertex_base, constant(line))))};
        if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW) {
            const u32 element{static_cast<u32>(attribute) -
                              static_cast<u32>(IR::Attribute::PositionX)};
            module.OpStore(module.OpAccessChain(output_f32, position, constant(element)), value);
            if (element == 2) {
                position_z = value;
            } else if (element == 3) {
                position_w = value;
            }
        } else if (IR::IsGeneric(attribute)) {
            module.OpStore(module.OpAccessChain(output_f32,
                                                generics[IR::GenericAttributeIndex(attribute)],
                                                constant(IR::GenericAttributeElement(attribute))),
                           value);
        }
    }
    // OpenGL-style clip space, as the mesh epilogue's ConvertDepth: z = (z + w) / 2.
    if (convert_depth && Sirit::ValidId(position_z) && Sirit::ValidId(position_w)) {
        const Id z{module.OpFMul(f32_type, module.OpFAdd(f32_type, position_z, position_w),
                                 module.Constant(f32_type, 0.5f))};
        module.OpStore(module.OpAccessChain(output_f32, position, constant(2)), z);
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", interfaces);
    return module.Assemble();
}

std::vector<u32> EmitMeshEmulationDistribution(const Profile& profile) {
    Sirit::Module module{profile.supported_spirv};
    module.AddCapability(spv::Capability::Shader);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    module.AddExtension("SPV_KHR_storage_buffer_storage_class");

    const Id void_type{module.TypeVoid()};
    const Id bool_type{module.TypeBool()};
    const Id u32_type{module.TypeInt(32, false)};
    const auto constant = [&](u32 value) { return module.Constant(u32_type, value); };

    const Id array_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
    const Id struct_type{module.TypeStruct(array_type)};
    module.Decorate(struct_type, spv::Decoration::Block);
    module.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
    const Id buffer_pointer{module.TypePointer(spv::StorageClass::StorageBuffer, struct_type)};
    const Id buffer_u32{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};
    std::vector<Id> interfaces;
    const auto define_buffer = [&](u32 binding, std::string_view name) {
        const Id buffer{module.AddGlobalVariable(buffer_pointer, spv::StorageClass::StorageBuffer)};
        module.Decorate(buffer, spv::Decoration::Binding, binding);
        module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
        module.Name(buffer, name);
        interfaces.push_back(buffer);
        return buffer;
    };
    const Id payload{define_buffer(0, "payload")};
    const Id offsets{define_buffer(1, "offsets")};
    const Id args{define_buffer(2, "args")};
    const Id counter{define_buffer(3, "arena_counter")};

    constexpr u32 num_constants{sizeof(MeshEmulationDistributionConstants) / sizeof(u32)};
    std::array<Id, num_constants> members;
    members.fill(u32_type);
    const Id constants_type{module.TypeStruct(std::span<const Id>(members))};
    module.Decorate(constants_type, spv::Decoration::Block);
    for (u32 member = 0; member < num_constants; ++member) {
        module.MemberDecorate(constants_type, member, spv::Decoration::Offset, member * 4);
    }
    const Id constants{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::PushConstant, constants_type),
        spv::StorageClass::PushConstant)};
    interfaces.push_back(constants);
    const Id constant_u32{module.TypePointer(spv::StorageClass::PushConstant, u32_type)};
    if (profile.supported_spirv < 0x00010400) {
        interfaces.clear(); // only Input and Output variables before SPIR-V 1.4
    }

    const auto word = [&](Id buffer, Id index) {
        return module.OpAccessChain(buffer_u32, buffer, constant(0), index);
    };
    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const Id function_u32{module.TypePointer(spv::StorageClass::Function, u32_type)};
    const Id total_var{module.AddLocalVariable(function_u32, spv::StorageClass::Function)};
    const Id task_var{module.AddLocalVariable(function_u32, spv::StorageClass::Function)};
    const auto push = [&](u32 member) {
        return module.OpLoad(u32_type, module.OpAccessChain(constant_u32, constants,
                                                            constant(member)));
    };
    const Id tasks{push(0)};
    const Id payload_words{push(1)};
    const Id capacity_words{push(2)};
    const Id block_words{push(3)};
    const Id vertices_per_group{push(4)};
    const Id max_groups{push(5)};
    module.OpStore(total_var, constant(0));
    module.OpStore(task_var, constant(0));

    // for (task = 0; task < tasks; ++task) { offsets[task] = total; total += count; }
    const Id header{module.OpLabel()};
    const Id condition_label{module.OpLabel()};
    const Id body_label{module.OpLabel()};
    const Id continue_label{module.OpLabel()};
    const Id merge{module.OpLabel()};
    module.OpBranch(header);
    module.AddLabel(header);
    module.OpLoopMerge(merge, continue_label, spv::LoopControlMask::MaskNone);
    module.OpBranch(condition_label);
    module.AddLabel(condition_label);
    const Id task{module.OpLoad(u32_type, task_var)};
    module.OpBranchConditional(module.OpULessThan(bool_type, task, tasks), body_label, merge);
    module.AddLabel(body_label);
    {
        const Id total{module.OpLoad(u32_type, total_var)};
        const Id requested{module.OpLoad(
            u32_type, word(payload, module.OpIAdd(u32_type,
                                                  module.OpIMul(u32_type, task, payload_words),
                                                  constant(1))))};
        module.OpStore(word(offsets, task), total);
        module.OpStore(total_var, module.OpIAdd(u32_type, total, requested));
    }
    module.OpBranch(continue_label);
    module.AddLabel(continue_label);
    module.OpStore(task_var, module.OpIAdd(u32_type, module.OpLoad(u32_type, task_var), constant(1)));
    module.OpBranch(header);
    module.AddLabel(merge);

    // The draw's blocks are allocated from the arena every draw of the submission shares. The
    // passes of consecutive draws are ordered by barriers, so a plain read and write allocate.
    // The counter keeps growing past the end, so the host sees how much the submission needed.
    const Id total{module.OpLoad(u32_type, total_var)};
    const Id base{module.OpLoad(u32_type, word(counter, constant(0)))};
    module.OpStore(word(counter, constant(0)),
                   module.OpIAdd(u32_type, base, module.OpIMul(u32_type, total, block_words)));
    const Id room{module.OpSelect(u32_type, module.OpULessThan(bool_type, base, capacity_words),
                                  module.OpISub(u32_type, capacity_words, base), constant(0))};
    const Id dispatched{module.OpUMin(
        u32_type, module.OpUMin(u32_type, total, module.OpUDiv(u32_type, room, block_words)),
        max_groups)};
    module.OpStore(word(args, constant(0)), dispatched);
    module.OpStore(word(args, constant(1)), constant(1));
    module.OpStore(word(args, constant(2)), constant(1));
    module.OpStore(word(args, constant(MESH_EMULATION_ARGS_REQUESTED)), total);
    const u32 draw{MESH_EMULATION_DRAW_ARGS_OFFSET / 4};
    module.OpStore(word(args, constant(draw)), module.OpIMul(u32_type, dispatched, vertices_per_group));
    module.OpStore(word(args, constant(draw + 1)), constant(1));
    module.OpStore(word(args, constant(draw + 2)), constant(0));
    module.OpStore(word(args, constant(draw + 3)), constant(0));
    module.OpStore(word(args, constant(MESH_EMULATION_ARGS_BASE)), base);
    module.OpStore(word(args, constant(MESH_EMULATION_ARGS_TASKS)), tasks);
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", interfaces);
    module.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1U, 1U, 1U);
    return module.Assemble();
}

} // namespace Shader::Backend::SPIRV
