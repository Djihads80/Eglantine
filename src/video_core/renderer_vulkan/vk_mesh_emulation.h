// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <unordered_map>
#include <array>
#include <mutex>
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
    u32 draw_vertices{}; ///< vertices of the direct draw of the draw being prepared

    /// Times the draws' compute work while it captures.
    FrameDump* frame_dump{};

    /// Breadcrumbs: a draw's number, and where its work notes reaching (start) and finishing
    /// (end) its dispatches. A device loss logs them and the last draws.
    struct Breadcrumb {
        u32 number;
        VkBuffer buffer;
    };
    [[nodiscard]] Breadcrumb NoteDraw(u64 pipeline, u32 groups, bool has_task, u32 capacity);

    /// Notes that the draw being prepared draws in the current render pass: the end of the pass
    /// leaves its breadcrumb (the GPU finished the pass it drew in).
    void NoteDrawInPass();

    /// The headers of emulated task draws are read back after their dispatches: the mesh
    /// workgroups their task programs launched size the slots of the pipeline's next draws
    /// (Fanout). EDEN_NX2_MESH_EMULATE_CHECK=1 reads back every draw's and logs them.
    [[nodiscard]] bool Checking() const noexcept {
        return checking;
    }

    /// Where the draw's header goes after its dispatches (a slot of the readback buffer), noted
    /// with what the host asked for; none when the readback slots are all pending.
    [[nodiscard]] std::pair<VkBuffer, VkDeviceSize> CheckSlot(u64 pipeline, u32 groups,
                                                              bool has_task, u32 capacity);

    /// Mesh workgroups per task workgroup the pipeline's draws get slots for: at least
    /// `minimum`, more once its task programs launched more.
    [[nodiscard]] u32 Fanout(u64 pipeline, u32 minimum);

    /// Slots a task draw of the pipeline draws with a direct draw: all of them until its draws
    /// were read back, then a quarter more than they used.
    [[nodiscard]] u32 DrawSlots(u64 pipeline, u32 groups, u32 capacity);

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
    struct NotedDraw {
        u32 number;
        u64 pipeline;
        u32 groups;
        u32 capacity;
        bool has_task;
        u64 tick;
    };
    void ReportLoss();

    vk::Buffer breadcrumb_buffer;
    std::mutex noted_mutex;
    std::array<NotedDraw, 32> noted{};
    u32 next_number{1};
    u32 pass_draw{}; ///< last emulated draw in the current render pass
    vk::Buffer check_buffer;
    bool checking{};
    std::vector<Check> checks;
    std::unordered_map<u64, u32> fanouts;        ///< learned per pipeline
    std::unordered_map<u64, u32> slots_per_task; ///< learned per pipeline
    u32 inconsistent_logged{};
    u32 next_check_slot{};
    u32 checks_logged{};
    u32 overflows_logged{};
    u32 learned_logged{};
    size_t current{};
    VkDeviceSize max_size{};
    VkDeviceSize alignment{};
};

} // namespace Vulkan
