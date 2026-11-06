// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Task/mesh emulation without VK_EXT_mesh_shader: the clamp pass between an emulated task
// dispatch and its mesh dispatch. One invocation checks the draw's header against what the host
// wrote in it and writes the mesh dispatch and the vertex shader's draw (MeshEmulationArgs),
// bounded by the draw's slots, ahead of the header, outside the region its programs write.

#include <array>

#include <sirit/sirit.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace Shader::Backend::SPIRV {

std::vector<u32> EmitMeshEmulationClamp() {
    using Sirit::Id;
    Sirit::Module module{0x00010000};
    module.AddCapability(spv::Capability::Shader);
    module.AddExtension("SPV_KHR_storage_buffer_storage_class");

    const Id void_type{module.TypeVoid()};
    const Id bool_type{module.TypeBool()};
    const Id u32_type{module.TypeInt(32, false)};
    const auto u32_constant = [&](u32 value) { return module.Constant(u32_type, value); };

    const Id array_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
    const Id struct_type{module.TypeStruct(array_type)};
    module.Decorate(struct_type, spv::Decoration::Block);
    module.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
    const Id buffer{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::StorageBuffer, struct_type),
        spv::StorageClass::StorageBuffer)};
    module.Decorate(buffer, spv::Decoration::Binding, 0U);
    module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
    module.Name(buffer, "mesh_emulation");
    const Id word_pointer{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};

    constexpr u32 num_constants{sizeof(MeshEmulationClampConstants) / sizeof(u32)};
    std::array<Id, num_constants> members;
    members.fill(u32_type);
    const Id constants_type{module.TypeStruct(std::span<const Id>(members))};
    module.Decorate(constants_type, spv::Decoration::Block);
    for (u32 member = 0; member < num_constants; ++member) {
        module.MemberDecorate(constants_type, member, spv::Decoration::Offset,
                              member * static_cast<u32>(sizeof(u32)));
    }
    const Id constants{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::PushConstant, constants_type),
        spv::StorageClass::PushConstant)};
    const Id constant_pointer{module.TypePointer(spv::StorageClass::PushConstant, u32_type)};

    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const auto constant = [&](u32 member) {
        return module.OpLoad(u32_type, module.OpAccessChain(constant_pointer, constants,
                                                            u32_constant(member)));
    };
    const auto word = [&](Id index) {
        return module.OpAccessChain(word_pointer, buffer, u32_constant(0), index);
    };
    const Id header{constant(0)};
    const Id capacity{constant(1)};
    const auto header_word = [&](u32 index) {
        return module.OpLoad(u32_type,
                             word(module.OpIAdd(u32_type, header, u32_constant(index))));
    };
    const Id zero{u32_constant(0)};
    const Id one{u32_constant(1)};
    // The host's header, its dispatch within its slots.
    const Id dispatched{header_word(MESH_EMULATION_DISPATCH_WORD)};
    Id valid{module.OpULessThanEqual(bool_type, dispatched, capacity)};
    const auto require = [&](Id condition) {
        valid = module.OpLogicalAnd(bool_type, valid, condition);
    };
    require(module.OpIEqual(bool_type, header_word(MESH_EMULATION_DISPATCH_WORD + 1), one));
    require(module.OpIEqual(bool_type, header_word(MESH_EMULATION_DISPATCH_WORD + 2), one));
    for (u32 index = 0; index < 5; ++index) {
        require(module.OpIEqual(bool_type, header_word(MESH_EMULATION_CAPACITY_WORD + index),
                                constant(1 + index)));
    }
    const Id slots{module.OpSelect(u32_type, valid, dispatched, zero)};
    const std::array<Id, 8> args{
        slots,
        one,
        one,
        module.OpSelect(u32_type, valid, zero, one),
        module.OpIMul(u32_type, slots, constant(6)),
        one,
        zero,
        zero,
    };
    for (u32 index = 0; index < args.size(); ++index) {
        module.OpStore(word(u32_constant(index)), args[index]);
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main");
    module.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1U, 1U, 1U);
    return module.Assemble();
}

} // namespace Shader::Backend::SPIRV
