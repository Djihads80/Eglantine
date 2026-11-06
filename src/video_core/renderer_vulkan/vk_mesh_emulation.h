// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class FrameDump;
class MemoryAllocator;
class Scheduler;

/// Switch 2 task/mesh draws are emulated on a device without VK_EXT_mesh_shader: the task and mesh
/// programs run as compute shaders writing a storage buffer, and a generated vertex shader draws
/// what they wrote with the pipeline's fragment program and state. EDEN_NX2_MESH_EMULATE=1
/// emulates them on every device (A/B against native mesh shaders), =0 never.
[[nodiscard]] bool MeshEmulationEnabled(const Device& device);

/// The compute stages of an emulated task/mesh pipeline and its storage buffer layout.
struct MeshEmulationStages {
    bool has_task{};
    vk::ShaderModule task_module; ///< freed once the pipelines are built
    vk::ShaderModule mesh_module;
    vk::Pipeline task_pipeline;
    vk::Pipeline mesh_pipeline;
    u32 task_threads{};
    u32 payload_words{};
    u32 attribute_words{}; ///< per output slot (mesh workgroup)
    u32 map_words{};       ///< per output slot
    u32 max_primitives{};
};

/// A region of the storage buffers emulated task/mesh draws write and draw from.
struct MeshEmulationRegion {
    VkBuffer buffer{};
    VkDeviceSize offset{};
};

/// The storage buffers emulated task/mesh draws write and draw from: each draw takes its own
/// region, from chunks reused once the GPU is done with the ticks that used them (a draw's compute
/// work never waits for an earlier draw's vertex shader).
class MeshEmulationBuffer {
public:
    explicit MeshEmulationBuffer(const Device& device, MemoryAllocator& memory_allocator,
                                 Scheduler& scheduler);
    ~MeshEmulationBuffer();

    MeshEmulationBuffer(const MeshEmulationBuffer&) = delete;
    MeshEmulationBuffer& operator=(const MeshEmulationBuffer&) = delete;

    /// A region of `size` bytes for the next emulated draw.
    [[nodiscard]] MeshEmulationRegion Allocate(VkDeviceSize size);

    /// Most bytes a draw's region may span (the device's storage buffer range).
    [[nodiscard]] VkDeviceSize MaxSize() const noexcept {
        return max_size;
    }

    /// The region of the draw being prepared (its draw command at MESH_EMULATION_DRAW_WORD);
    /// a null buffer when the draw is not emulated.
    MeshEmulationRegion draw{};

    /// Times the draws' compute work while it captures.
    FrameDump* frame_dump{};

    /// EDEN_NX2_MESH_EMULATE_CHECK=1: the headers of emulated draws are read back after their
    /// dispatches and logged (mesh workgroups launched, dispatched and drawn).
    [[nodiscard]] bool Checking() const noexcept {
        return static_cast<bool>(check_buffer);
    }

    /// Where the draw's header goes after its dispatches (a slot of the readback buffer), noted
    /// with what the host asked for.
    [[nodiscard]] std::pair<VkBuffer, VkDeviceSize> CheckSlot(u64 pipeline, u32 groups,
                                                              bool has_task, u32 capacity);

private:
    struct Chunk {
        vk::Buffer buffer;
        VkDeviceSize size{};
        VkDeviceSize used{};
        u64 tick{}; ///< of its last region
    };

    struct Check {
        u64 tick;
        u64 pipeline;
        u32 slot;
        u32 groups;
        u32 capacity;
        bool has_task;
    };

    /// Logs the checks whose tick is done.
    void ReportChecks();

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    std::vector<Chunk> chunks;
    vk::Buffer check_buffer;
    std::vector<Check> checks;
    u32 next_check_slot{};
    u32 checks_logged{};
    u32 overflows_logged{};
    size_t current{};
    VkDeviceSize max_size{};
    VkDeviceSize alignment{};
};

} // namespace Vulkan
