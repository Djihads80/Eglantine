// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include "common/common_types.h"
#include "video_core/game_dlss_status.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class MemoryAllocator;

/// Checks a game's own DLSS network output against its input on the GPU
/// (host_shaders/dlss_network_probe.comp) and reads the results back a few frames later, so the
/// check never waits for the GPU.
class DlssNetworkProbe final : public ComputePass {
public:
    explicit DlssNetworkProbe(const Device& device, Scheduler& scheduler,
                              DescriptorPool& descriptor_pool,
                              ComputePassDescriptorQueue& descriptor_queue,
                              MemoryAllocator& memory_allocator);
    ~DlssNetworkProbe();

    /// Records a check of output against input (sampled images in the GENERAL layout) and returns
    /// the result of the oldest earlier check the GPU has finished, if any. Skips recording while
    /// every result slot is still in flight.
    [[nodiscard]] std::optional<VideoCore::GameDlssProbe> Probe(VkImageView input,
                                                                VkImageView output);

    static constexpr u32 GRID_WIDTH = 32;
    static constexpr u32 GRID_HEIGHT = 18;

private:
    static constexpr u32 SLOTS = 4;
    static constexpr VkDeviceSize SLOT_SIZE = GRID_WIDTH * GRID_HEIGHT * 4 * sizeof(f32);

    [[nodiscard]] VideoCore::GameDlssProbe Read(u32 slot) const;

    Scheduler& scheduler;
    ComputePassDescriptorQueue& descriptor_queue;
    vk::Sampler sampler;
    vk::Buffer results;
    std::array<u64, SLOTS> ticks{}; ///< The tick each slot's check was recorded in (0: free).
    u32 oldest{};                   ///< The slot whose result comes next.
    u32 next{};                     ///< The slot the next check is recorded into.
};

} // namespace Vulkan
