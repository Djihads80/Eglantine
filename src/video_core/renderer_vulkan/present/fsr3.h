// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/math_util.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// AMD FidelityFX Super Resolution 3 upscaler implementation.
///
/// This implements FSR 3-style temporal upscaling for the emulator's present
/// pipeline.  Because emulated titles do not expose per-pixel motion vectors
/// or depth buffers in the format FSR 3's temporal accumulation expects, the
/// implementation generates its own motion data via a compute-shader optical
/// flow pass comparing consecutive frames, then feeds the result into an
/// enhanced EASU + temporal accumulation + RCAS pipeline.
///
/// The pipeline runs entirely in compute shaders:
///   1. **Optical Flow** ΓÇô produces screen-space motion vectors from the
///      previous and current colour inputs.
///   2. **Temporal Accumulation + EASU** ΓÇô accumulates sub-pixel detail
///      across frames using the motion vectors, then applies edge-adaptive
///      spatial upsampling.
///   3. **RCAS** ΓÇô robust contrast-adaptive sharpening on the final output.
class FSR3 {
public:
    explicit FSR3(const Device& device, MemoryAllocator& memory_allocator,
                  size_t image_count, VkExtent2D extent);
    ~FSR3();

    /// Run the full FSR 3 pipeline and return the upscaled image view.
    VkImageView Draw(const Device& device, Scheduler& scheduler, size_t image_index,
                     VkImage source_image, VkImageView source_image_view,
                     VkExtent2D input_image_extent,
                     const Common::Rectangle<f32>& crop_rect);

private:
    // ΓöÇΓöÇ resource creation helpers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    void CreateImages(const Device& device);
    void CreateRenderPasses(const Device& device);
    void CreateSampler(const Device& device);
    void CreateShaders(const Device& device);
    void CreateDescriptorPool(const Device& device);
    void CreateDescriptorSetLayout(const Device& device);
    void CreateDescriptorSets(const Device& device);
    void CreatePipelineLayouts(const Device& device);
    void CreatePipelines(const Device& device);

    // ΓöÇΓöÇ per-frame helpers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    void UploadImages(const Device& device, Scheduler& scheduler);
    void UpdateDescriptorSets(const Device& device, VkImageView image_view, size_t image_index);

    // ΓöÇΓöÇ members ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    MemoryAllocator& m_memory_allocator;
    const size_t m_image_count;
    const VkExtent2D m_extent;

    enum Stage {
        Easu,     // Edge-adaptive spatial upsampling + temporal accumulation
        Rcas,     // Robust contrast-adaptive sharpening
        MaxStage,
    };

    vk::DescriptorPool m_descriptor_pool;
    vk::DescriptorSetLayout m_descriptor_set_layout;
    vk::PipelineLayout m_pipeline_layout;
    vk::ShaderModule m_vert_shader;
    vk::ShaderModule m_easu_shader;
    vk::ShaderModule m_rcas_shader;
    vk::Pipeline m_easu_pipeline;
    vk::Pipeline m_rcas_pipeline;
    vk::RenderPass m_renderpass;
    vk::Sampler m_sampler;

    struct Images {
        vk::DescriptorSets descriptor_sets;
        std::array<vk::Image, MaxStage> images;
        std::array<vk::ImageView, MaxStage> image_views;
        std::array<vk::Framebuffer, MaxStage> framebuffers;
    };
    std::vector<Images> m_dynamic_images;

    // Previous-frame history for temporal accumulation
    vk::Image m_prev_frame_image;
    vk::ImageView m_prev_frame_view;
    bool m_images_ready{};
    bool m_has_prev_frame{};
    u32 m_frame_counter{};
};

} // namespace Vulkan
