// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>

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
}

MeshEmulationBuffer::~MeshEmulationBuffer() = default;

VkBuffer MeshEmulationBuffer::Request(VkDeviceSize request) {
    std::erase_if(retired, [this](const auto& entry) { return scheduler.IsFree(entry.first); });
    if (request <= size) {
        return *buffer;
    }
    if (buffer) {
        // Draws recorded before may still read it.
        retired.emplace_back(scheduler.CurrentTick(), std::move(buffer));
    }
    size = std::bit_ceil(std::max<VkDeviceSize>(request, 1ULL << 20));
    buffer = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::DeviceLocal);
    LOG_INFO(Render_Vulkan, "Task/mesh emulation buffer: {} KiB", size / 1024);
    return *buffer;
}

} // namespace Vulkan
