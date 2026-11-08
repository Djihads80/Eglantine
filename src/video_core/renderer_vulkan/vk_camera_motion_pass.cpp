// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_camera_motion_pass.h"

#include "video_core/host_shaders/camera_motion_vectors_comp_spv.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

constexpr u32 GROUP_SIZE = 8;

struct PushConstants {
    std::array<f32, 16> reprojection;
    std::array<s32, 2> size;
};

constexpr std::array<VkDescriptorSetLayoutBinding, 2> BINDINGS{{
    {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr std::array<VkDescriptorUpdateTemplateEntry, 2> TEMPLATES{{
    {
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .offset = 0,
        .stride = sizeof(DescriptorUpdateEntry),
    },
    {
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .offset = sizeof(DescriptorUpdateEntry),
        .stride = sizeof(DescriptorUpdateEntry),
    },
}};

constexpr DescriptorBankInfo BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 0,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = 1,
    .images = 1,
    .score = 2,
};

constexpr VkPushConstantRange PUSH_CONSTANT_RANGE{
    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    .offset = 0,
    .size = static_cast<u32>(sizeof(PushConstants)),
};

constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

} // Anonymous namespace

CameraMotionPass::CameraMotionPass(const Device& device_, Scheduler& scheduler_,
                                   DescriptorPool& descriptor_pool,
                                   ComputePassDescriptorQueue& descriptor_queue_,
                                   MemoryAllocator& memory_allocator_)
    : ComputePass(device_, scheduler_, descriptor_pool, BINDINGS, TEMPLATES, BANK_INFO,
                  PUSH_CONSTANT_RANGE, CAMERA_MOTION_VECTORS_COMP_SPV),
      scheduler{scheduler_}, descriptor_queue{descriptor_queue_},
      memory_allocator{memory_allocator_} {
    sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

CameraMotionPass::~CameraMotionPass() = default;

void CameraMotionPass::CreateImage(VkExtent2D size) {
    if (image) {
        scheduler.Finish(); // the old image may still be in use
    }
    image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    extent = size;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([handle = *image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = handle,
                                   .subresourceRange = COLOR_RANGE,
                               });
    });
}

void CameraMotionPass::Record(VkImageView depth, VkExtent2D size,
                              const std::array<f32, 16>& reprojection) {
    if (!image || size.width != extent.width || size.height != extent.height) {
        CreateImage(size);
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    descriptor_queue.Acquire(scheduler, 2);
    descriptor_queue.AddSampledImage(depth, *sampler);
    descriptor_queue.AddImage(*view);
    const void* const descriptor_data{descriptor_queue.UpdateData()};
    const PushConstants constants{
        .reprojection = reprojection,
        .size = {static_cast<s32>(size.width), static_cast<s32>(size.height)},
    };
    scheduler.Record([this, descriptor_data, constants, size](vk::CommandBuffer cmdbuf) {
        // The scene's draws wrote the depth; the last evaluation read the motion vectors.
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((size.width + GROUP_SIZE - 1) / GROUP_SIZE,
                        (size.height + GROUP_SIZE - 1) / GROUP_SIZE, 1);
    });
    // The pass binds its own pipeline and descriptors on the command buffer.
    scheduler.InvalidateState();
}

} // namespace Vulkan
