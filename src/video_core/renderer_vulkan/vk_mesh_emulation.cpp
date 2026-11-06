// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_mesh_emulation.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

bool MeshEmulationEnabled(const Device& device) {
    static const int forced = [] {
        const char* const value = std::getenv("EDEN_NX2_MESH_EMULATE");
        if (value == nullptr || value[0] == '\0') {
            return -1;
        }
        return value[0] == '0' ? 0 : 1;
    }();
    if (forced >= 0) {
        return forced == 1;
    }
    return !device.IsExtMeshShaderSupported();
}

MeshEmulationBuffer::MeshEmulationBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                                         Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {
    const VkDeviceSize range = device.GetMaxStorageBufferRange();
    max_size = std::min<VkDeviceSize>(range != 0 ? range : (128ULL << 20), 256ULL << 20);
    alignment = std::max<VkDeviceSize>(device.GetStorageBufferAlignment(), 256);
}

MeshEmulationBuffer::~MeshEmulationBuffer() = default;

MeshEmulationRegion MeshEmulationBuffer::Allocate(VkDeviceSize request) {
    const VkDeviceSize size = Common::AlignUp(request, alignment);
    const u64 tick = scheduler.CurrentTick();
    const auto take = [&](size_t index) {
        Chunk& chunk = chunks[index];
        const VkDeviceSize offset = chunk.used;
        chunk.used += size;
        chunk.tick = tick;
        current = index;
        return MeshEmulationRegion{*chunk.buffer, offset};
    };
    if (current < chunks.size() && chunks[current].used + size <= chunks[current].size) {
        return take(current);
    }
    // A chunk the GPU is done with.
    for (size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index].size >= size && scheduler.IsFree(chunks[index].tick)) {
            chunks[index].used = 0;
            return take(index);
        }
    }
    const VkDeviceSize chunk_size = std::bit_ceil(std::max<VkDeviceSize>(size, 4ULL << 20));
    chunks.push_back(Chunk{
        .buffer = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = chunk_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::DeviceLocal),
        .size = chunk_size,
    });
    LOG_INFO(Render_Vulkan, "Task/mesh emulation buffers: {} of {} KiB", chunks.size(),
             chunk_size / 1024);
    return take(chunks.size() - 1);
}

} // namespace Vulkan
