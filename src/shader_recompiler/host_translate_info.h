// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Shader {

// Try to keep entries here to a minimum
// They can accidentally change the cached information in a shader

/// Misc information about the host
struct HostTranslateInfo {
    static constexpr u32 DEFAULT_DESCRIPTOR_LIMIT = 1024;

    u64 min_ssbo_alignment{};            ///< Minimum alignment supported by the device for SSBOs
    u32 max_per_stage_descriptor_sampled_images{}; ///< maximum sampled descriptors per stage
    u32 max_per_stage_resources{};                 ///< maximum resources per stage
    u32 max_per_stage_descriptor_storage_buffers{}; ///< maximum storage buffers per stage
    /// Maximum samplers per stage, which a combined image sampler (a texture) counts against too
    u32 max_per_stage_descriptor_samplers{};
    u32 max_descriptor_set_samplers{};
    u32 max_descriptor_set_uniform_buffers{};
    u32 max_descriptor_set_uniform_buffers_dynamic{};
    u32 max_descriptor_set_storage_buffers{};
    u32 max_descriptor_set_storage_buffers_dynamic{};
    u32 max_descriptor_set_sampled_images{};
    u32 max_descriptor_set_storage_images{};
    u32 max_descriptor_set_input_attachements{};
    bool support_float64{};      ///< True when the device supports 64-bit floats
    bool support_float16{};      ///< True when the device supports 16-bit floats
    bool support_int64{};        ///< True when the device supports 64-bit integers
    bool needs_demote_reorder{}; ///< True when the device needs DemoteToHelperInvocation reordered
    bool support_snorm_render_buffer{};  ///< True when the device supports SNORM render buffers
    bool support_viewport_index_layer{}; ///< True when the device supports gl_Layer in VS
    bool support_viewport_mask{};
    bool support_geometry_shader_passthrough{}; ///< True when the device supports geometry
                                                ///< passthrough shaders
    bool support_conditional_barrier{}; ///< True when the device supports barriers in conditional
                                        ///< control flow
    bool single_lane_geometry_subgroups{};
    /// True when a handle read from a table at a per-invocation index may become a descriptor
    /// array of the table's entries (not OpenGL: its pipelines bind at most 64 textures; Vulkan
    /// only with nonuniform sampled image array indexing)
    bool support_handle_table_arrays{};

    /// The properties that change which values translation reads from the environment, a bit
    /// each: those deciding a pass that reads values, or a pass that changes the program before
    /// one does. A disk cache record made where they differ can lack a value read here and still
    /// hold every value read there. The descriptor limits decide which storage buffer passes run,
    /// but each of those asks what the first one asked. A property that starts deciding such a pass
    /// is added at the end, so that the bits of the others stay.
    [[nodiscard]] u32 EnvironmentReadProperties() const noexcept {
        u32 properties{};
        u32 bit{1};
        for (const bool property :
             {support_float64, support_float16, support_int64, support_conditional_barrier,
              needs_demote_reorder, support_geometry_shader_passthrough, support_viewport_mask,
              single_lane_geometry_subgroups, support_snorm_render_buffer,
              support_handle_table_arrays}) {
            properties |= property ? bit : 0U;
            bit <<= 1;
        }
        return properties;
    }

    void ApplyDescriptorLimitPolicy() noexcept {
        if (min_ssbo_alignment == 0) {
            min_ssbo_alignment = 1;
        }
        ApplyDescriptorLimitFallback(max_per_stage_descriptor_sampled_images);
        ApplyDescriptorLimitFallback(max_per_stage_resources);
        ApplyDescriptorLimitFallback(max_per_stage_descriptor_storage_buffers);
        ApplyDescriptorLimitFallback(max_per_stage_descriptor_samplers);
        ApplyDescriptorLimitFallback(max_descriptor_set_samplers);
        ApplyDescriptorLimitFallback(max_descriptor_set_uniform_buffers);
        ApplyDescriptorLimitFallback(max_descriptor_set_uniform_buffers_dynamic);
        ApplyDescriptorLimitFallback(max_descriptor_set_storage_buffers);
        ApplyDescriptorLimitFallback(max_descriptor_set_storage_buffers_dynamic);
        ApplyDescriptorLimitFallback(max_descriptor_set_sampled_images);
        ApplyDescriptorLimitFallback(max_descriptor_set_storage_images);
        ApplyDescriptorLimitFallback(max_descriptor_set_input_attachements);
    }

private:
    static void ApplyDescriptorLimitFallback(u32& limit) noexcept {
        if (limit == 0) {
            limit = DEFAULT_DESCRIPTOR_LIMIT;
        }
    }
};

} // namespace Shader
