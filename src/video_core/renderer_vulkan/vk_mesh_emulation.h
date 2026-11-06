// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

/// Switch 2 task/mesh draws are emulated on a device without VK_EXT_mesh_shader: the task and mesh
/// programs run as compute shaders writing a storage buffer, and a generated vertex shader draws
/// what they wrote with the pipeline's fragment program and state. EDEN_NX2_MESH_EMULATE=1
/// emulates them on every device (A/B against native mesh shaders), =0 never.
[[nodiscard]] bool MeshEmulationEnabled(const Device& device);

/// The compute stages of an emulated task/mesh pipeline and its storage buffer layout.
struct MeshEmulationStages {
    vk::ShaderModule task_module; ///< null without a task program
    vk::ShaderModule mesh_module;
    vk::Pipeline task_pipeline;
    vk::Pipeline mesh_pipeline;
    u32 task_threads{};
    u32 payload_words{};
    u32 attribute_words{}; ///< per output slot (mesh workgroup)
    u32 map_words{};       ///< per output slot
    u32 max_primitives{};
};

/// The storage buffer emulated task/mesh draws write and draw from. Their compute work is
/// recorded outside the render pass right before each draw, so the draws share one buffer: each
/// waits for the one before (a barrier) before writing it.
class MeshEmulationBuffer {
public:
    explicit MeshEmulationBuffer(const Device& device, MemoryAllocator& memory_allocator,
                                 Scheduler& scheduler);
    ~MeshEmulationBuffer();

    MeshEmulationBuffer(const MeshEmulationBuffer&) = delete;
    MeshEmulationBuffer& operator=(const MeshEmulationBuffer&) = delete;

    /// A buffer of at least `size` bytes for the next emulated draw.
    [[nodiscard]] VkBuffer Request(VkDeviceSize size);

    /// Most bytes a draw's buffer may span (the device's storage buffer range).
    [[nodiscard]] VkDeviceSize MaxSize() const noexcept {
        return max_size;
    }

    /// The buffer of the draw being prepared (its draw command at MESH_EMULATION_DRAW_WORD);
    /// null when the draw is not emulated.
    VkBuffer draw_buffer{};

private:
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    vk::Buffer buffer;
    VkDeviceSize size{};
    VkDeviceSize max_size{};
    std::vector<std::pair<u64, vk::Buffer>> retired; ///< outgrown buffers and their last tick
};

} // namespace Vulkan
