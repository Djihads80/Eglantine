// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Before any Vulkan header: those pull in Xlib, whose Success macro breaks nvdata.h.
#include "video_core/renderer_vulkan/vk_dlss_network_probe.h"

#include "video_core/host_shaders/dlss_network_probe_comp_spv.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

constexpr std::array<VkDescriptorSetLayoutBinding, 3> BINDINGS{{
    {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr std::array<VkDescriptorUpdateTemplateEntry, 3> TEMPLATES{{
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
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .offset = sizeof(DescriptorUpdateEntry),
        .stride = sizeof(DescriptorUpdateEntry),
    },
    {
        .dstBinding = 2,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = 2 * sizeof(DescriptorUpdateEntry),
        .stride = sizeof(DescriptorUpdateEntry),
    },
}};

constexpr DescriptorBankInfo BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 1,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = 2,
    .images = 0,
    .score = 3,
};

/// A block's output brightness may differ from its input's by this share of the input (plus a
/// floor for dark blocks) before it counts as mismatched. Working networks stay below 0.5 in every
/// block of Fortune's Weave's title and Kunlun Base; frozen ones exceed it in 6-10% of the blocks.
constexpr f32 MISMATCH = 0.5f;
constexpr f32 DARK_FLOOR = 0.01f;

} // Anonymous namespace

DlssNetworkProbe::DlssNetworkProbe(const Device& device_, Scheduler& scheduler_,
                                   DescriptorPool& descriptor_pool,
                                   ComputePassDescriptorQueue& descriptor_queue_,
                                   MemoryAllocator& memory_allocator)
    : ComputePass(device_, scheduler_, descriptor_pool, BINDINGS, TEMPLATES, BANK_INFO, {},
                  DLSS_NETWORK_PROBE_COMP_SPV),
      scheduler{scheduler_}, descriptor_queue{descriptor_queue_} {
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
    results = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = SLOT_SIZE * SLOTS,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Download);
}

DlssNetworkProbe::~DlssNetworkProbe() = default;

std::optional<VideoCore::GameDlssProbe> DlssNetworkProbe::Probe(VkImageView input,
                                                                VkImageView output) {
    std::optional<VideoCore::GameDlssProbe> result;
    if (ticks[oldest] != 0 && scheduler.IsFree(ticks[oldest])) {
        result = Read(oldest);
        ticks[oldest] = 0;
        oldest = (oldest + 1) % SLOTS;
    }
    if (ticks[next] != 0) {
        return result;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    descriptor_queue.Acquire(scheduler, 3);
    descriptor_queue.AddSampledImage(input, *sampler);
    descriptor_queue.AddSampledImage(output, *sampler);
    descriptor_queue.AddBuffer(*results, SLOT_SIZE * next, SLOT_SIZE);
    const void* const descriptor_data{descriptor_queue.UpdateData()};
    scheduler.Record([this, descriptor_data](vk::CommandBuffer cmdbuf) {
        // The network's last pass and the game's draws wrote the images.
        const VkMemoryBarrier read_images{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, read_images);
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.Dispatch(GRID_WIDTH, GRID_HEIGHT, 1);
        const VkMemoryBarrier read_results{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                               read_results);
    });
    ticks[next] = scheduler.CurrentTick();
    next = (next + 1) % SLOTS;
    return result;
}

VideoCore::GameDlssProbe DlssNetworkProbe::Read(u32 slot) const {
    results.Invalidate();
    std::array<f32, GRID_WIDTH * GRID_HEIGHT * 4> blocks;
    std::memcpy(blocks.data(), results.Mapped().data() + SLOT_SIZE * slot, sizeof(blocks));
    VideoCore::GameDlssProbe probe{.blocks = GRID_WIDTH * GRID_HEIGHT};
    for (u32 block = 0; block < GRID_WIDTH * GRID_HEIGHT; ++block) {
        const f32 input = blocks[block * 4];
        const f32 output = blocks[block * 4 + 1];
        probe.invalid_samples += static_cast<u32>(blocks[block * 4 + 2]);
        const f32 difference = std::abs(output - input) / (std::max(input, 0.0f) + DARK_FLOOR);
        if (!std::isfinite(difference) || difference > MISMATCH) {
            ++probe.mismatched;
        }
        if (std::isfinite(difference)) {
            probe.worst = std::max(probe.worst, difference);
        }
    }
    return probe;
}

} // namespace Vulkan
