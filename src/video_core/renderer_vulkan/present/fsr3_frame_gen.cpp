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

// Copies the presented frame (GENERAL) into one of the frame slots (GENERAL), which the shaders
// read afterwards.
void CopyFrame(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination, VkExtent2D extent) {
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
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, after);
}

VkWriteDescriptorSet StorageImageWrite(VkDescriptorSet set, u32 binding,
                                       const VkDescriptorImageInfo* info) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = info,
    };
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
    WriteFrameDescriptors(device);
    m_initialized = true;
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    for (size_t slot = 0; slot < m_frames.size(); ++slot) {
        m_frames[slot] = CreateWrappedImage(m_allocator, m_extent, m_format);
        m_frame_views[slot] = CreateWrappedImageView(device, m_frames[slot], m_format);
    }

    // Half-resolution motion vector image
    m_motion_image = CreateWrappedImage(m_allocator, m_flow_extent, VK_FORMAT_R16G16_SFLOAT);
    m_motion_view = CreateWrappedImageView(device, m_motion_image, VK_FORMAT_R16G16_SFLOAT);

    // Flow: 2 sets of 2 sampled images and 1 storage image. Interpolation: 2 * MaxTargets sets
    // of 3 sampled images and 1 storage image.
    constexpr u32 flow_sets = 2;
    constexpr u32 interp_sets = 2 * MaxTargets;
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             flow_sets * 2 + interp_sets * 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, flow_sets + interp_sets},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = flow_sets + interp_sets,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    // Optical flow
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

    // Frame interpolation
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

    const std::vector<VkDescriptorSetLayout> flow_layouts(2, *m_flow_desc_layout);
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, flow_layouts);
    const std::vector<VkDescriptorSetLayout> interp_layouts(2 * MaxTargets, *m_interp_desc_layout);
    m_interp_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, interp_layouts);
}

void Fsr3FrameGen::WriteFrameDescriptors(const Device& device) {
    // Everything but the interpolation's destination: with slot s current, the other slot holds
    // the previous frame.
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(2 * 2 + 2 * MaxTargets * 3);
    const VkDescriptorImageInfo motion_store{
        .imageView = *m_motion_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    std::vector<VkWriteDescriptorSet> updates;
    for (size_t slot = 0; slot < m_frames.size(); ++slot) {
        const VkImageView current = *m_frame_views[slot];
        const VkImageView previous = *m_frame_views[1 - slot];
        const VkDescriptorSet flow = m_flow_desc_sets[slot];
        updates.push_back(CreateWriteDescriptorSet(image_infos, *m_linear_sampler, current, flow, 0));
        updates.push_back(CreateWriteDescriptorSet(image_infos, *m_linear_sampler, previous, flow, 1));
        updates.push_back(StorageImageWrite(flow, 2, &motion_store));

        for (size_t target = 0; target < MaxTargets; ++target) {
            const VkDescriptorSet interp = m_interp_desc_sets[slot * MaxTargets + target];
            updates.push_back(
                CreateWriteDescriptorSet(image_infos, *m_linear_sampler, current, interp, 0));
            updates.push_back(
                CreateWriteDescriptorSet(image_infos, *m_linear_sampler, previous, interp, 1));
            updates.push_back(
                CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_motion_view, interp, 2));
        }
    }
    device.GetLogical().UpdateDescriptorSets(updates, {});
}

void Fsr3FrameGen::Process(const Device& device, Scheduler& scheduler, VkImage current_frame) {
    if (!m_has_history) {
        // First frame: it is both the previous and the current one
        scheduler.Record([this, current_frame](vk::CommandBuffer cmdbuf) {
            const std::array initial{
                MakeBarrier(*m_frames[0], 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL),
                MakeBarrier(*m_frames[1], 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL),
                MakeBarrier(*m_motion_image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, initial);
            CopyFrame(cmdbuf, current_frame, *m_frames[0], m_extent);
            CopyFrame(cmdbuf, current_frame, *m_frames[1], m_extent);
        });
        m_newest = 0;
        m_has_history = true;
        return;
    }

    // The new frame replaces the older one, the other slot keeps the previous frame
    const size_t slot = 1 - m_newest;
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

    scheduler.Record([this, constants, current_frame, slot, flow_w, flow_h,
                      flow_set = m_flow_desc_sets[slot]](vk::CommandBuffer cmdbuf) {
        CopyFrame(cmdbuf, current_frame, *m_frames[slot], m_extent);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipe_layout, 0,
                                  flow_set, {});
        cmdbuf.PushConstants(*m_flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((flow_w + 15) / 16, (flow_h + 15) / 16, 1);

        // The motion stays in GENERAL, as its descriptors say
        const auto motion = MakeBarrier(*m_motion_image, VK_ACCESS_SHADER_WRITE_BIT,
                                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, motion);
    });

    m_newest = slot;
    m_flow_valid = true;
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler, u32 target,
                            VkImage destination, VkImageView destination_storage_view,
                            float blend_factor, float flow_scale) {
    if (!m_flow_valid || target >= MaxTargets) {
        return;
    }

    const size_t index = m_newest * MaxTargets + target;
    const VkDescriptorSet interp_set = m_interp_desc_sets[index];
    if (m_interp_dest_views[index] != destination_storage_view) {
        const VkDescriptorImageInfo dest_info{
            .imageView = destination_storage_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        device.GetLogical().UpdateDescriptorSets(
            std::array{StorageImageWrite(interp_set, 3, &dest_info)}, {});
        m_interp_dest_views[index] = destination_storage_view;
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

    scheduler.Record([this, constants, out_w, out_h, interp_set,
                      destination](vk::CommandBuffer cmdbuf) {
        // The whole frame is written: drop its old contents (shown by an earlier present)
        const auto discard = MakeBarrier(destination, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, discard);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0,
                                  interp_set, {});
        cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);

        const auto written = MakeBarrier(
            destination, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, written);
    });
}

} // namespace Vulkan
