// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_optical_flow_comp_spv.h"

namespace Vulkan {

namespace {

// ΓöÇΓöÇ Precise pipeline barriers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

void BarrierComputeWriteToRead(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

// ΓöÇΓöÇ Batched dual-copy ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
// Copy one source image into TWO destinations with only 3 barrier calls
// instead of 6 (avoiding redundant source transitions).
void CopyImageToTwo(vk::CommandBuffer cmdbuf, VkImage source,
                    VkImage dest_a, VkImage dest_b, VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    // 1. Transition source ΓåÆ TRANSFER_SRC, both dests ΓåÆ TRANSFER_DST (batched)
    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(dest_a, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        MakeBarrier(dest_b, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    // 2. Copy source ΓåÆ dest_a, then source ΓåÆ dest_b (source stays TRANSFER_SRC)
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_a,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_b,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    // 3. Transition all three back to GENERAL (batched)
    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_a, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_b, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}

// Single-image copy (for first-frame init)
[[maybe_unused]] void CopyImageToImage(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination,
                      VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(destination, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}

} // anonymous namespace

struct FlowPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float search_radius;
    float block_size;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float blend_factor;
    float flow_scale;
};

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent},
      m_flow_extent{std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)},
      m_format{format} {
    CreateResources(device);
    CreateShaders(device);
    CreatePipelines(device);
    m_initialized = true;
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    m_history_image = CreateWrappedImage(m_allocator, m_extent, m_format);
    m_history_view = CreateWrappedImageView(device, m_history_image, m_format);

    // Current-frame copy for interpolation shader
    m_current_image = CreateWrappedImage(m_allocator, m_extent, m_format);
    m_current_view = CreateWrappedImageView(device, m_current_image, m_format);

    // Half-resolution motion vector image
    m_motion_image = CreateWrappedImage(m_allocator, m_flow_extent, VK_FORMAT_R16G16_SFLOAT);
    m_motion_view = CreateWrappedImageView(device, m_motion_image, VK_FORMAT_R16G16_SFLOAT);

    // Descriptor pool ΓÇö sized for flow (3 bindings) + interp (4 bindings) ├ù 2 sets
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 4,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    // ΓöÇΓöÇ Optical Flow Pipeline ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    const std::array flow_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_flow_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(flow_bindings.size()),
        .pBindings = flow_bindings.data(),
    });

    const VkPushConstantRange flow_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FlowPushConstants),
    };

    m_flow_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_flow_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &flow_push_range,
    });

    m_flow_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_flow_shader,
            .pName = "main",
        },
        .layout = *m_flow_pipe_layout,
    });

    // ΓöÇΓöÇ Frame Interpolation Pipeline ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    const std::array interp_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_interp_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(interp_bindings.size()),
        .pBindings = interp_bindings.data(),
    });

    const VkPushConstantRange interp_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(InterpPushConstants),
    };

    m_interp_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_interp_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &interp_push_range,
    });

    m_interp_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_interp_shader,
            .pName = "main",
        },
        .layout = *m_interp_pipe_layout,
    });

    // Allocate descriptor sets
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_flow_desc_layout});
    m_interp_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_interp_desc_layout});
}

void Fsr3FrameGen::Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                           VkImageView current_view, u64 frame_count) {
    if (!m_has_history) {
        // First frame: populate history and current-frame copy via batched dual-copy
        scheduler.Record([this, current_frame](vk::CommandBuffer cmdbuf) {
            CopyImageToTwo(cmdbuf, current_frame, *m_history_image, *m_current_image, m_extent);
        });
        m_has_history = true;
        m_last_frame_count = frame_count;
        return;
    }

    // ΓöÇΓöÇ Duplicate-frame detection ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (frame_count == m_last_frame_count && m_flow_valid) {
        return;
    }
    m_last_frame_count = frame_count;

    // ΓöÇΓöÇ Descriptor caching ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (current_view != m_cached_flow_current_view) {
        std::vector<VkDescriptorImageInfo> image_infos;
        image_infos.reserve(4);
        std::vector<VkWriteDescriptorSet> updates{
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, current_view, m_flow_desc_sets[0], 0),
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_history_view, m_flow_desc_sets[0], 1),
        };

        const VkDescriptorImageInfo motion_store_info{
            .imageView = *m_motion_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        updates.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_flow_desc_sets[0],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &motion_store_info,
        });
        device.GetLogical().UpdateDescriptorSets(updates, {});
        m_cached_flow_current_view = current_view;
    }

    // Dispatch Optical Flow at half resolution
    const u32 flow_w = m_flow_extent.width;
    const u32 flow_h = m_flow_extent.height;
    const FlowPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(flow_w),
        .inv_render_h = 1.0f / static_cast<float>(flow_h),
        .render_w = static_cast<float>(flow_w),
        .render_h = static_cast<float>(flow_h),
        .search_radius = 2.0f,
        .block_size = 2.0f,
    };

    scheduler.Record([this, constants, current_frame, flow_w, flow_h](vk::CommandBuffer cmdbuf) {
        // Transition motion image to GENERAL for storage write
        TransitionImageLayout(cmdbuf, *m_motion_image, VK_IMAGE_LAYOUT_GENERAL);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipe_layout, 0,
                                  m_flow_desc_sets[0], {});
        cmdbuf.PushConstants(*m_flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((flow_w + 15) / 16, (flow_h + 15) / 16, 1);

        // Motion vectors: GENERAL ΓåÆ SHADER_READ_ONLY
        BarrierComputeWriteToRead(cmdbuf, *m_motion_image);

        // Batched dual-copy: source ΓåÆ history + current (3 barriers instead of 6)
        CopyImageToTwo(cmdbuf, current_frame, *m_history_image, *m_current_image, m_extent);
    });

    m_flow_valid = true;
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler,
                            VkImageView destination_storage_view, float blend_factor,
                            float flow_scale) {
    if (!m_has_history || !m_flow_valid) {
        return;
    }

    // ΓöÇΓöÇ Descriptor caching for interpolation ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (destination_storage_view != m_cached_interp_dest_view) {
        std::vector<VkDescriptorImageInfo> image_infos;
        image_infos.reserve(4);
        std::vector<VkWriteDescriptorSet> updates{
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_current_view, m_interp_desc_sets[0], 0),
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_history_view, m_interp_desc_sets[0], 1),
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_motion_view, m_interp_desc_sets[0], 2),
        };

        const VkDescriptorImageInfo dest_info{
            .imageView = destination_storage_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        updates.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_interp_desc_sets[0],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &dest_info,
        });
        device.GetLogical().UpdateDescriptorSets(updates, {});
        m_cached_interp_dest_view = destination_storage_view;
    }

    // Interpolation dispatches at full resolution
    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .blend_factor = blend_factor,
        .flow_scale = flow_scale,
    };

    scheduler.Record([this, constants, out_w, out_h](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0,
                                  m_interp_desc_sets[0], {});
        cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);
    });
}

} // namespace Vulkan
