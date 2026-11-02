// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorPool;
class Device;
class MemoryAllocator;
class Scheduler;
class StagingBufferPool;

/// AMD FidelityFX Super Resolution 3.1 (the FSR 3.1.4 upscaler from FidelityFX SDK v1.1.4, shaders
/// in externals/FidelityFX-SDK) in place of a game's DLSS evaluation. The host side follows the
/// SDK's ffx_fsr3upscaler.cpp: the same passes, internal images and constants, recorded through the
/// scheduler. Inputs are taken as reversed depth, render-size motion vectors and HDR colour, and
/// the exposure is computed by FSR itself.
class Fsr3Upscaler {
public:
    struct Inputs {
        VkImageView color;          ///< Jittered linear colour at the render size.
        VkImageView depth;          ///< Device depth (reversed) at the render size.
        VkImageView motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        /// This frame's sample offset in render pixels: input texel k sampled k + 0.5 - jitter.
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels (the render size for UV offsets).
        std::array<f32, 2> motion_vector_scale;
        f32 frame_time_ms;
        bool reset;
        bool sharpen;
        f32 sharpness; ///< 0 to 1 as FSR takes it: 2 stops of RCAS at 0, none at 1.
    };

    explicit Fsr3Upscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator, StagingBufferPool& staging_buffer_pool,
                          DescriptorPool& descriptor_pool,
                          ComputePassDescriptorQueue& descriptor_queue);
    ~Fsr3Upscaler();

    /// Whether the device has what the passes need (formats, shader features).
    [[nodiscard]] static bool IsSupported(const Device& device);

    /// Records one evaluation into Output(): RGBA16F at the upscale size, GENERAL layout, ready to
    /// be sampled by a fragment shader.
    void Upscale(const Inputs& inputs);

    [[nodiscard]] VkImage OutputImage() const noexcept;
    [[nodiscard]] VkImageView OutputView() const noexcept;

    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading Output().
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }

private:
    class Pass;

    struct Texture {
        vk::Image image;
        vk::ImageView view;                ///< Every level.
        std::vector<vk::ImageView> levels; ///< One view per level, for images with mips.
    };

    void CreateResources(VkExtent2D render_size, VkExtent2D upscale_size);
    Texture CreateTexture(VkFormat format, u32 width, u32 height, u32 levels = 1);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& descriptor_queue;

    vk::Sampler point_sampler;
    vk::Sampler linear_sampler;
    std::vector<std::unique_ptr<Pass>> passes;

    // Ping-pong pairs, indexed by frame parity like the SDK's resource tables.
    std::array<Texture, 2> accumulation;
    std::array<Texture, 2> luma;
    std::array<Texture, 2> luma_history;
    std::array<Texture, 2> internal_upscaled;
    Texture farthest_depth; ///< Also the luma instability (the SDK aliases them).
    Texture shading_change;
    Texture new_locks;
    Texture spd_mips;
    Texture farthest_depth_mip1;
    Texture dilated_motion_vectors;
    Texture dilated_depth;
    Texture previous_nearest_depth;
    Texture dilated_reactive_masks;
    Texture frame_info;
    Texture spd_atomic;
    Texture lanczos_lut;
    Texture default_reactive;
    Texture output;

    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    std::array<s32, 2> previous_render_size{};
    std::array<s32, 2> previous_upscale_size{};
    std::array<f32, 2> previous_jitter{};
    f32 jitter_phase_count{};
    f32 frame_index{};
    u32 frame_parity{};
    bool first_execution{true};
};

} // namespace Vulkan
