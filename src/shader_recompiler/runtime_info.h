// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <map>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/varying_state.h"

namespace Shader {

enum class AttributeType : u8 {
    Float,
    SignedInt,
    UnsignedInt,
    SignedScaled,
    UnsignedScaled,
    Disabled,
};

enum class InputTopology {
    Points,
    Lines,
    LinesAdjacency,
    Triangles,
    TrianglesAdjacency,
};

struct InputTopologyVertices {
    static u32 vertices(InputTopology input_topology) {
        switch (input_topology) {
        case InputTopology::Lines:
            return 2;
        case InputTopology::LinesAdjacency:
            return 4;
        case InputTopology::Triangles:
            return 3;
        case InputTopology::TrianglesAdjacency:
            return 6;
        case InputTopology::Points:
        default:
            return 1;
        }
    }
};

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessThanEqual,
    Greater,
    NotEqual,
    GreaterThanEqual,
    Always,
};

enum class TessPrimitive {
    Isolines,
    Triangles,
    Quads,
};

enum class TessSpacing {
    Equal,
    FractionalOdd,
    FractionalEven,
};

struct TransformFeedbackVarying {
    u32 buffer{};
    u32 stream{};
    u32 stride{};
    u32 offset{};
    u32 components{};
};

struct RuntimeInfo {
    std::array<AttributeType, 32> generic_input_types{};
    VaryingState previous_stage_stores;
    /// Fragment stage after a mesh program: generic inputs it writes per primitive.
    std::bitset<32> per_primitive_generics{};
    std::map<IR::Attribute, IR::Attribute> previous_stage_legacy_stores_mapping;

    bool convert_depth_mode{};
    bool force_early_z{};

    TessPrimitive tess_primitive{};
    TessSpacing tess_spacing{};
    bool tess_clockwise{};

    InputTopology input_topology{};

    std::optional<float> fixed_state_point_size;
    std::optional<CompareFunction> alpha_test_func;
    float alpha_test_reference{};

    /// Static Y negate value
    bool y_negate{};
    /// Use storage buffers instead of global pointers on GLASM
    bool glasm_use_storage_buffers{};

    /// Transform feedback state for each varying
    std::array<TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_count{0};

    /// Output types for each color attachment
    std::array<AttributeType, 8> color_output_types{};

    /// Dual source blending
    bool dual_source_blend{};

    /// Mesh program on a device without VK_EXT_mesh_shader: emit it as a compute shader that
    /// writes its vertices and primitives to the storage buffer at mesh_output_binding (see
    /// EmitMeshEmulationVertex for the layout), for a vertex shader to draw them.
    bool mesh_as_compute{};
    u32 mesh_output_binding{};
    /// Mesh emulation with a task program (mesh_as_compute on both). The task's compute shader
    /// writes its payload (count at word 1) to the payload buffer, task_payload_words words per
    /// task workgroup. Each mesh workgroup finds its task by a binary search of the offsets
    /// buffer (first mesh workgroup of each task) and copies that task's payload. The arguments
    /// buffer holds the draw's output base and task count (MESH_EMULATION_ARGS_*). ~0 when there
    /// is no task program.
    u32 mesh_task_payload_binding{~0U};
    u32 mesh_task_offsets_binding{~0U};
    u32 mesh_task_args_binding{~0U};
    u32 mesh_task_payload_stride{}; ///< payload buffer words per task workgroup
};

} // namespace Shader
