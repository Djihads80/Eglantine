// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/accelerated_swizzle.h"

namespace VideoCommon {
struct SwizzleParameters;
}

namespace Vulkan {

using VideoCommon::Accelerated::BlockLinearSwizzle3DParams;

class Device;
class StagingBufferPool;
class Scheduler;
class Image;
struct StagingBufferRef;

class ComputePass {
public:
    explicit ComputePass(const Device& device, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                         vk::Span<VkDescriptorSetLayoutBinding> bindings,
                         vk::Span<VkDescriptorUpdateTemplateEntry> templates,
                         const DescriptorBankInfo& bank_info,
                         vk::Span<VkPushConstantRange> push_constants, std::span<const u32> code,
                         std::optional<u32> optional_subgroup_size = std::nullopt);
    ~ComputePass();

protected:
    const Device& device;
    vk::DescriptorUpdateTemplate descriptor_template;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;

private:
    vk::ShaderModule module;
};

class Uint8Pass final : public ComputePass {
public:
    explicit Uint8Pass(const Device& device_, Scheduler& scheduler_,
                       DescriptorPool& descriptor_pool_, StagingBufferPool& staging_buffer_pool_,
                       ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~Uint8Pass();

    /// Assemble uint8 indices into an uint16 index buffer
    /// Returns a pair with the staging buffer, and the offset where the assembled data is
    std::pair<VkBuffer, VkDeviceSize> Assemble(u32 num_vertices, VkBuffer src_buffer,
                                               u32 src_offset);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class QuadIndexedPass final : public ComputePass {
public:
    explicit QuadIndexedPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~QuadIndexedPass();

    std::pair<VkBuffer, VkDeviceSize> Assemble(
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices,
        u32 base_vertex, VkBuffer src_buffer, u32 src_offset, bool is_strip);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class ConditionalRenderingResolvePass final : public ComputePass {
public:
    explicit ConditionalRenderingResolvePass(
        const Device& device_, Scheduler& scheduler_, DescriptorPool& descriptor_pool_,
        ComputePassDescriptorQueue& compute_pass_descriptor_queue_);

    void Resolve(VkBuffer dst_buffer, VkBuffer src_buffer, u32 src_offset, bool compare_to_zero);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class QueriesPrefixScanPass final : public ComputePass {
public:
    explicit QueriesPrefixScanPass(const Device& device_, Scheduler& scheduler_,
                                   DescriptorPool& descriptor_pool_,
                                   ComputePassDescriptorQueue& compute_pass_descriptor_queue_);

    void Run(VkBuffer accumulation_buffer, VkBuffer dst_buffer, VkBuffer src_buffer,
             size_t number_of_sums, size_t min_accumulation_limit, size_t max_accumulation_limit);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class ASTCDecoderPass final : public ComputePass {
public:
    explicit ASTCDecoderPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_,
                             MemoryAllocator& memory_allocator_);
    ~ASTCDecoderPass();

    void Assemble(Image& image, const StagingBufferRef& map,
                  std::span<const VideoCommon::SwizzleParameters> swizzles);

    /// Decodes only `layers` of `image` (an initialized array); `map` holds them in order.
    void AssembleLayers(Image& image, const StagingBufferRef& map,
                        std::span<const VideoCommon::SwizzleParameters> swizzles,
                        std::span<const u32> layers);

    /// Where a decode writes: an RGBA8 image with a storage view per (host) level.
    struct Target {
        VkImage image;
        VkImageAspectFlags aspect_mask;
        bool initialized;
        std::function<VkImageView(s32 host_level)> level_view;
        bool wait; ///< Wait for the GPU afterwards (the decoded image itself is sampled).
        /// When set, the input holds only these layers (in order), each decoded into
        /// layer_view(host level, layer).
        std::span<const u32> layers{};
        std::function<VkImageView(s32 host_level, u32 layer)> layer_view{};
    };

    /// Decodes `info`'s ASTC data in `map` into `target`. `skip` leading levels are absent on the
    /// host (TrimmedLevels) and `guest_size_bytes` is the full guest size.
    void AssembleInto(const VideoCommon::ImageInfo& info, u32 skip, size_t guest_size_bytes,
                      const Target& target, const StagingBufferRef& map,
                      std::span<const VideoCommon::SwizzleParameters> swizzles);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
    MemoryAllocator& memory_allocator;
};

/// Compresses an RGBA8 image level to BC1 or BC3 blocks in a buffer (bc_encoder.comp).
class BcEncoderPass final : public ComputePass {
public:
    explicit BcEncoderPass(const Device& device_, Scheduler& scheduler_,
                           DescriptorPool& descriptor_pool_,
                           ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~BcEncoderPass();

    /// Encodes every layer of one level (`source` is its RGBA8 storage view) into `buffer`,
    /// `out_offset` bytes past `buffer_offset`; blocks are row-major, layer after layer.
    void Encode(VkImageView source, u32 width, u32 height, u32 layers, bool bc3, VkBuffer buffer,
                VkDeviceSize buffer_offset, VkDeviceSize buffer_size, VkDeviceSize out_offset);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class BlockLinearUnswizzle3DPass final : public ComputePass {
public:
    explicit BlockLinearUnswizzle3DPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~BlockLinearUnswizzle3DPass();

    void Unswizzle(Image& image,
                   const StagingBufferRef& swizzled,
                   std::span<const VideoCommon::SwizzleParameters> swizzles,
                   u32 z_start, u32 z_count);

    void UnswizzleChunk(
        Image& image,
        const StagingBufferRef& swizzled,
        const VideoCommon::SwizzleParameters& sw,
        const BlockLinearSwizzle3DParams& params,
        u32 blocks_x, u32 blocks_y,
        u32 z_start, u32 z_count);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

} // namespace Vulkan
