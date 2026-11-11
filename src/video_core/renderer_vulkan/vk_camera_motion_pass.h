// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class MemoryAllocator;

/// Motion vectors from the camera alone (host_shaders/camera_motion_vectors.comp), for games that
/// give an upscaler none: render-pixel offsets to the previous frame, RG16F in the GENERAL layout.
class CameraMotionPass final : public ComputePass {
public:
    static constexpr VkFormat FORMAT = VK_FORMAT_R16G16_SFLOAT;

    explicit CameraMotionPass(const Device& device, Scheduler& scheduler,
                              DescriptorPool& descriptor_pool,
                              ComputePassDescriptorQueue& descriptor_queue,
                              MemoryAllocator& memory_allocator);
    ~CameraMotionPass();

    /// Records the motion of every pixel of a depth image (a depth-only view in the GENERAL
    /// layout) of the given size; reprojection is a column-major matrix from (pixel x, pixel y,
    /// device depth, 1) to the previous frame's pixel.
    void Record(VkImageView depth, VkExtent2D size, const std::array<f32, 16>& reprojection);

    [[nodiscard]] VkImage Image() const noexcept {
        return *image;
    }
    [[nodiscard]] VkImageView View() const noexcept {
        return *view;
    }

private:
    void CreateImage(VkExtent2D size);

    Scheduler& scheduler;
    ComputePassDescriptorQueue& descriptor_queue;
    MemoryAllocator& memory_allocator;
    vk::Sampler sampler;
    vk::Image image;
    vk::ImageView view;
    VkExtent2D extent{};
};

} // namespace Vulkan
