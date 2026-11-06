// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Task/mesh emulation without VK_EXT_mesh_shader: the vertex shader that draws an emulated mesh
// program's output. The draw has max_primitives triangles per output slot (mesh workgroup);
// vertex i is corner i % 3 of the slot's primitive i / 3 % max_primitives. Its vertex comes from
// the slot's map region, its values from the slot's attribute region (the stage buffer's lines),
// and primitives past the slot's count are moved off screen. No vertex is shared, so per-primitive
// values reach a flat fragment input whichever vertex provokes.

#include <array>
#include <optional>

#include <sirit/sirit.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/attribute.h"

namespace Shader::Backend::SPIRV {

std::vector<u32> EmitMeshEmulationVertex(const Profile& profile, const MeshInfo& mesh, u32 binding,
                                         bool convert_depth) {
    using Sirit::Id;
    Sirit::Module module{profile.supported_spirv};
    module.AddCapability(spv::Capability::Shader);
    module.AddExtension("SPV_KHR_storage_buffer_storage_class");

    const Id void_type{module.TypeVoid()};
    const Id bool_type{module.TypeBool()};
    const Id u32_type{module.TypeInt(32, false)};
    const Id f32_type{module.TypeFloat(32)};
    const Id vec4_type{module.TypeVector(f32_type, 4)};
    const auto u32_constant = [&](u32 value) { return module.Constant(u32_type, value); };
    const auto f32_constant = [&](f32 value) { return module.Constant(f32_type, value); };

    std::vector<Id> interfaces;
    const auto variable = [&](Id type, spv::StorageClass storage_class) {
        const Id id{module.AddGlobalVariable(module.TypePointer(storage_class, type), storage_class)};
        if (storage_class == spv::StorageClass::Input ||
            storage_class == spv::StorageClass::Output || profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(id);
        }
        return id;
    };

    const Id array_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
    const Id struct_type{module.TypeStruct(array_type)};
    module.Decorate(struct_type, spv::Decoration::Block);
    module.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
    module.MemberDecorate(struct_type, 0, spv::Decoration::NonWritable);
    const Id buffer{variable(struct_type, spv::StorageClass::StorageBuffer)};
    module.Decorate(buffer, spv::Decoration::Binding, binding);
    module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
    module.Name(buffer, "mesh_emulation");
    const Id word_pointer{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};

    const Id vertex_index{variable(u32_type, spv::StorageClass::Input)};
    module.Decorate(vertex_index, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    const Id position{variable(vec4_type, spv::StorageClass::Output)};
    module.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);

    // What a line feeds: a position or generic component (per vertex or per primitive).
    struct Target {
        std::optional<u32> generic;
        u32 element{};
    };
    const auto target_of = [](u32 line) -> std::optional<Target> {
        const IR::Attribute attribute{line};
        if (IR::IsGeneric(attribute)) {
            return Target{IR::GenericAttributeIndex(attribute),
                          IR::GenericAttributeElement(attribute)};
        }
        if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW) {
            return Target{std::nullopt, static_cast<u32>(attribute) -
                                            static_cast<u32>(IR::Attribute::PositionX)};
        }
        return std::nullopt; // no vertex shader output for it (layer, viewport, ...)
    };
    std::array<Id, IR::NUM_GENERICS> generics{};
    const auto define_generic = [&](u32 line) {
        const std::optional<Target> target{target_of(line)};
        if (!target || !target->generic || Sirit::ValidId(generics[*target->generic])) {
            return;
        }
        const u32 index{*target->generic};
        generics[index] = variable(vec4_type, spv::StorageClass::Output);
        module.Decorate(generics[index], spv::Decoration::Location, index);
    };
    for (const u32 line : mesh.vertex_lines) {
        define_generic(line);
    }
    for (const u32 line : mesh.primitive_lines) {
        define_generic(line);
    }

    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const auto load_word = [&](Id word) {
        return module.OpLoad(u32_type,
                             module.OpAccessChain(word_pointer, buffer, u32_constant(0), word));
    };
    const auto add = [&](Id a, Id b) { return module.OpIAdd(u32_type, a, b); };
    const auto mul = [&](Id a, u32 b) { return module.OpIMul(u32_type, a, u32_constant(b)); };

    const u32 max_primitives{(std::max)(mesh.max_primitives, 1U)};
    const u32 max_vertices{(std::max)(mesh.max_vertices, 1U)};
    const Id index{module.OpLoad(u32_type, vertex_index)};
    const Id primitive_index{module.OpUDiv(u32_type, index, u32_constant(3))};
    const Id corner{module.OpISub(u32_type, index, mul(primitive_index, 3))};
    const Id slot{module.OpUDiv(u32_type, primitive_index, u32_constant(max_primitives))};
    const Id primitive{module.OpISub(u32_type, primitive_index, mul(slot, max_primitives))};

    const Id map{add(load_word(u32_constant(MESH_EMULATION_MAP_WORD)),
                     mul(slot, MeshEmulationMapWords(mesh)))};
    const Id count{module.OpUMin(u32_type, load_word(add(map, u32_constant(MeshMapBytes(mesh)))),
                                 u32_constant(max_primitives))};
    // Past the slot's count, or a slot the mesh dispatch did not reach (a direct draw of more
    // slots than were dispatched: their regions hold another draw's data).
    const Id dispatched{load_word(u32_constant(MESH_EMULATION_DISPATCH_WORD))};
    const Id culled{module.OpLogicalOr(bool_type,
                                       module.OpUGreaterThanEqual(bool_type, primitive, count),
                                       module.OpUGreaterThanEqual(bool_type, slot, dispatched))};
    const Id vertex_byte{add(add(mul(primitive, 3), corner), u32_constant(4))};
    const Id vertex{module.OpUMin(
        u32_type, module.OpBitwiseAnd(u32_type, load_word(add(map, vertex_byte)), u32_constant(0xff)),
        u32_constant(max_vertices - 1))};

    // Line 0 of an element: (element / 32) * lines * 32 + element % 32; line n is 32n further.
    const Id attributes{add(load_word(u32_constant(MESH_EMULATION_ATTRIBUTE_WORD)),
                            mul(slot, MeshAttributeWords(mesh)))};
    const auto first_word = [&](Id base, Id element, u32 num_lines) {
        const Id group{module.OpShiftRightLogical(u32_type, element, u32_constant(5))};
        const Id lane{module.OpBitwiseAnd(u32_type, element, u32_constant(31))};
        return add(base, add(mul(group, num_lines * 32), lane));
    };
    std::array<Id, 4> position_values{f32_constant(0.0f), f32_constant(0.0f), f32_constant(0.0f),
                                      f32_constant(1.0f)};
    std::array<std::array<Id, 4>, IR::NUM_GENERICS> generic_values{};
    const auto read_lines = [&](const auto& lines, Id first) {
        for (u32 line = 0; line < lines.size(); ++line) {
            const std::optional<Target> target{target_of(lines[line])};
            if (!target) {
                continue;
            }
            const Id value{
                module.OpBitcast(f32_type, load_word(add(first, u32_constant(line * 32))))};
            if (target->generic) {
                generic_values[*target->generic][target->element] = value;
            } else {
                position_values[target->element] = value;
            }
        }
    };
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    const u32 primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
    read_lines(mesh.vertex_lines, first_word(attributes, vertex, vertex_lines));
    if (primitive_lines != 0) {
        read_lines(mesh.primitive_lines,
                   first_word(add(attributes, u32_constant(MeshPrimitiveBase(mesh))), primitive,
                              primitive_lines));
    }
    if (convert_depth) {
        // OpenGL-style clip space: z = (z + w) / 2, as the native mesh program does.
        position_values[2] = module.OpFMul(
            f32_type, module.OpFAdd(f32_type, position_values[2], position_values[3]),
            f32_constant(0.5f));
    }
    // Primitives past the count: all three corners at one point outside the clip volume.
    static constexpr std::array<f32, 4> off_screen{2.0f, 2.0f, 0.0f, 1.0f};
    for (size_t element = 0; element < 4; ++element) {
        position_values[element] = module.OpSelect(f32_type, culled,
                                                   f32_constant(off_screen[element]),
                                                   position_values[element]);
    }
    module.OpStore(position, module.OpCompositeConstruct(vec4_type, position_values));
    for (size_t generic = 0; generic < generics.size(); ++generic) {
        if (!Sirit::ValidId(generics[generic])) {
            continue;
        }
        std::array<Id, 4> values{generic_values[generic]};
        for (Id& value : values) {
            if (!Sirit::ValidId(value)) {
                value = f32_constant(0.0f);
            }
        }
        module.OpStore(generics[generic], module.OpCompositeConstruct(vec4_type, values));
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", interfaces);
    return module.Assemble();
}

} // namespace Shader::Backend::SPIRV
