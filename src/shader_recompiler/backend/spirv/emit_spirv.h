// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(RenderAreaLayout, render_area);

// The draw's first vertex is the first task (or mesh, without a task) workgroup index, as in NVN
// and GL_NV_mesh_shader. Only the first active stage reads non-zero. EDEN_NX2_MESH_FIRST=0: off.
struct MeshDrawBaseLayout {
    u32 task;
    u32 mesh;
};
constexpr u32 MESH_DRAW_BASE_OFFSET = sizeof(RescalingLayout);
static_assert(MESH_DRAW_BASE_OFFSET + sizeof(MeshDrawBaseLayout) <=
              sizeof(RescalingLayout) + sizeof(RenderAreaLayout));

inline bool MeshFirstEnabled() {
    static const bool enabled{[] {
        const char* const value{std::getenv("EDEN_NX2_MESH_FIRST")};
        return value == nullptr || value[0] != '0';
    }()};
    return enabled;
}

// Task/mesh emulation (RuntimeInfo::mesh_emulation): the storage buffer of an emulated draw
// starts with this header, written by the host before the task or mesh dispatch. Then, at the
// offsets it names (in words): the task payloads (payload_words per task workgroup), the mesh
// workgroups' (task workgroup, index) pairs, and per output slot (mesh workgroup) its attribute
// region (Shader::MeshAttributeWords) and its map region (Shader::MeshMapBytes words, one per
// byte, then the primitive count).
struct MeshEmulationHeader {
    std::array<u32, 3> dispatch;   ///< mesh workgroups to dispatch after a task program
    u32 task_allocated;            ///< mesh workgroups the task programs launched
    std::array<u32, 4> draw;       ///< VkDrawIndirectCommand of the vertex shader's draw
    u32 capacity;                  ///< output slots
    u32 payload_offset;
    u32 task_map_offset;
    u32 attribute_offset;
    u32 map_offset;
    std::array<u32, 3> reserved;
};
static_assert(sizeof(MeshEmulationHeader) == 64);
constexpr u32 MESH_EMULATION_DISPATCH_WORD = 0;
constexpr u32 MESH_EMULATION_ALLOCATED_WORD = 3;
constexpr u32 MESH_EMULATION_DRAW_WORD = 4;
constexpr u32 MESH_EMULATION_CAPACITY_WORD = 8;
constexpr u32 MESH_EMULATION_PAYLOAD_WORD = 9;
constexpr u32 MESH_EMULATION_TASK_MAP_WORD = 10;
constexpr u32 MESH_EMULATION_ATTRIBUTE_WORD = 11;
constexpr u32 MESH_EMULATION_MAP_WORD = 12;

/// Words per output slot of an emulated mesh program's map region (the map bytes, then the
/// primitive count).
[[nodiscard]] inline u32 MeshEmulationMapWords(const MeshInfo& mesh) {
    return MeshMapBytes(mesh) + 1;
}

/// The vertex shader of an emulated mesh draw: its vertex index is a corner of one of the
/// max_primitives primitives of an output slot, read from the storage buffer at `binding`.
[[nodiscard]] std::vector<u32> EmitMeshEmulationVertex(const Profile& profile,
                                                       const MeshInfo& mesh, u32 binding,
                                                       bool convert_depth);

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

} // namespace Shader::Backend::SPIRV
