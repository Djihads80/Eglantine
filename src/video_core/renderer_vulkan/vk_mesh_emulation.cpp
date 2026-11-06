// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_mesh_emulation.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr u32 CHECK_SLOTS = 256;
}

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
    const char* const check = std::getenv("EDEN_NX2_MESH_EMULATE_CHECK");
    if (check != nullptr && check[0] == '1') {
        check_buffer = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = CHECK_SLOTS * sizeof(Shader::Backend::SPIRV::MeshEmulationHeader),
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }
}

std::pair<VkBuffer, VkDeviceSize> MeshEmulationBuffer::CheckSlot(u64 pipeline, u32 groups,
                                                                 bool has_task, u32 capacity) {
    ReportChecks();
    if (checks.size() >= CHECK_SLOTS) {
        return {};
    }
    // A slot no pending check uses.
    u32 slot = next_check_slot;
    while (std::ranges::any_of(checks, [slot](const Check& entry) { return entry.slot == slot; })) {
        slot = (slot + 1) % CHECK_SLOTS;
    }
    next_check_slot = (slot + 1) % CHECK_SLOTS;
    checks.push_back({scheduler.CurrentTick(), pipeline, slot, groups, capacity, has_task});
    return {*check_buffer, slot * sizeof(Shader::Backend::SPIRV::MeshEmulationHeader)};
}

void MeshEmulationBuffer::ReportChecks() {
    if (!check_buffer || checks.empty()) {
        return;
    }
    bool invalidated{};
    std::erase_if(checks, [&](const Check& entry) {
        if (!scheduler.IsFree(entry.tick)) {
            return false;
        }
        if (!std::exchange(invalidated, true)) {
            check_buffer.Invalidate();
        }
        Shader::Backend::SPIRV::MeshEmulationHeader header;
        std::memcpy(&header, check_buffer.Mapped().data() + entry.slot * sizeof(header),
                    sizeof(header));
        const bool overflow = entry.has_task && header.task_allocated > entry.capacity;
        if ((overflow && overflows_logged++ < 32) || checks_logged++ < 64) {
            LOG_INFO(Render_Vulkan,
                     "Emulated mesh draw {:016x} check: {} {} workgroups, {} mesh workgroups "
                     "launched, {} dispatched of {} slots, {} vertices drawn{}",
                     entry.pipeline, entry.groups, entry.has_task ? "task" : "mesh",
                     entry.has_task ? header.task_allocated : entry.groups,
                     entry.has_task ? header.dispatch[0] : entry.groups, entry.capacity,
                     header.draw[0], overflow ? " (past the slots: dropped)" : "");
        }
        return true;
    });
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
