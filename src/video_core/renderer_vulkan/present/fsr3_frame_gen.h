// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// FSR 3 style frame generation: compute-based optical flow between the last two presented
/// frames and a bilateral interpolation between them, without engine motion vectors.
///
///  - Half-resolution optical flow
///  - The last two frames in two slots: each new frame replaces the older one
///  - Precise pipeline barriers instead of full-pipeline TransitionImageLayout
///  - One interpolation descriptor set per slot and presentation frame
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    /// Keeps the presented frame and measures the motion from the previous one.
    void Process(const Device& device, Scheduler& scheduler, VkImage current_frame);

    /// Writes the frame blend_factor (0 to 1) of the way from the previous frame to the current
    /// one into the presentation frame with that index.
    void Generate(const Device& device, Scheduler& scheduler, u32 target, VkImage destination,
                  VkImageView destination_storage_view, float blend_factor, float flow_scale);

    [[nodiscard]] bool IsReady() const {
        return m_initialized && m_flow_valid;
    }

private:
    static constexpr size_t MaxTargets = 8;

    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    void WriteFrameDescriptors(const Device& device);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkExtent2D m_flow_extent;   // half-res for optical flow
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    // Per slot: the flow from the other slot's frame to this one's
    vk::DescriptorSets m_flow_desc_sets;
    // Per slot and target, slot * MaxTargets + target
    vk::DescriptorSets m_interp_desc_sets;
    std::array<VkImageView, 2 * MaxTargets> m_interp_dest_views{};

    // The last two presented frames; m_newest is the current one, the other the previous one
    std::array<vk::Image, 2> m_frames;
    std::array<vk::ImageView, 2> m_frame_views;
    size_t m_newest{};

    vk::Image m_motion_image;
    vk::ImageView m_motion_view;

    bool m_initialized{};
    bool m_has_history{};
    bool m_flow_valid{};
};

} // namespace Vulkan
