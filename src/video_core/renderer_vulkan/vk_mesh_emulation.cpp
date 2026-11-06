// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>

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
constexpr u64 MAX_FANOUT = 1024;
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
    breadcrumb_buffer = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 3 * sizeof(u32),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Download);
    device.SetLossReporter([this] { ReportLoss(); });
    scheduler.SetRenderPassEndHook([this] {
        if (pass_draw == 0) {
            return;
        }
        scheduler.Record([buffer = *breadcrumb_buffer, number = pass_draw](vk::CommandBuffer cmdbuf) {
            cmdbuf.FillBuffer(buffer, 2 * sizeof(u32), sizeof(u32), number);
        });
        pass_draw = 0;
    });
    const char* const check = std::getenv("EDEN_NX2_MESH_EMULATE_CHECK");
    checking = check != nullptr && check[0] == '1';
    {
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

MeshEmulationBuffer::Breadcrumb MeshEmulationBuffer::NoteDraw(u64 pipeline, u32 groups,
                                                              bool has_task, u32 capacity) {
    std::scoped_lock lock{noted_mutex};
    const u32 number = next_number++;
    noted[number % noted.size()] = {number, pipeline, groups, capacity, has_task,
                                    scheduler.CurrentTick()};
    return {number, *breadcrumb_buffer};
}

void MeshEmulationBuffer::NoteDrawInPass() {
    pass_draw = next_number - 1;
}

void MeshEmulationBuffer::ReportLoss() {
    std::array<u32, 3> reached{};
    breadcrumb_buffer.Invalidate();
    std::memcpy(reached.data(), breadcrumb_buffer.Mapped().data(), sizeof(reached));
    LOG_CRITICAL(Render_Vulkan,
                 "Emulated task/mesh draws at the device loss: the GPU reached the work of draw "
                 "{} and finished that of draw {}, and finished the render pass of draw {}; {} "
                 "were recorded",
                 reached[0], reached[1], reached[2], next_number - 1);
    std::scoped_lock lock{noted_mutex};
    for (u32 back = 0; back < noted.size(); ++back) {
        const u32 number = next_number - 1 - back;
        const NotedDraw& draw = noted[number % noted.size()];
        if (number == 0 || draw.number != number) {
            break;
        }
        LOG_CRITICAL(Render_Vulkan, "  draw {}: pipeline {:016x}, {} {} workgroups, {} slots, tick {}",
                     draw.number, draw.pipeline, draw.groups, draw.has_task ? "task" : "mesh",
                     draw.capacity, draw.tick);
    }
}

u32 MeshEmulationBuffer::DrawSlots(u64 pipeline, u32 groups, u32 capacity) {
    const auto it = slots_per_task.find(pipeline);
    if (it == slots_per_task.end()) {
        return capacity;
    }
    // A quarter more than the most used per task workgroup so far: slots past the ones a draw
    // used are zeroed, their primitives culled.
    const u64 used = u64{groups} * it->second;
    return static_cast<u32>(std::min<u64>(capacity, used + used / 4 + 4));
}

u32 MeshEmulationBuffer::Fanout(u64 pipeline, u32 minimum) {
    ReportChecks();
    const auto it = fanouts.find(pipeline);
    return it != fanouts.end() ? std::max(it->second, minimum) : minimum;
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
        // A header the draw's own work cannot have left (its dispatch and launches bounded by
        // its slots): log it whole, learn nothing from it.
        const bool inconsistent =
            entry.has_task && (header.dispatch[0] > entry.capacity ||
                               header.task_allocated > u64{entry.groups} * MAX_FANOUT ||
                               header.capacity != entry.capacity);
        if (inconsistent) {
            if (inconsistent_logged++ < 16) {
                const auto* const words = reinterpret_cast<const u32*>(&header);
                LOG_WARNING(Render_Vulkan,
                            "Emulated mesh draw {:016x}: inconsistent header after its work ({} "
                            "task workgroups, {} slots): {:08x} {:08x} {:08x} {:08x} | {:08x} "
                            "{:08x} {:08x} {:08x} | {:08x} {:08x} {:08x} {:08x} {:08x} | {:08x} "
                            "{:08x} {:08x}",
                            entry.pipeline, entry.groups, entry.capacity, words[0], words[1],
                            words[2], words[3], words[4], words[5], words[6], words[7], words[8],
                            words[9], words[10], words[11], words[12], words[13], words[14],
                            words[15]);
            }
            return true;
        }
        if (entry.has_task && entry.groups != 0) {
            // Slots the pipeline's draws used per task workgroup (draws are sized by them).
            const u64 used = (u64{header.dispatch[0]} + entry.groups - 1) / entry.groups;
            u32& slots = slots_per_task[entry.pipeline];
            slots = std::max(slots, static_cast<u32>(used));
            // A quarter more than the most launched per task workgroup so far, in eights.
            const u64 per_task = (u64{header.task_allocated} + entry.groups - 1) / entry.groups;
            const u32 wanted = static_cast<u32>(
                std::min<u64>((per_task + per_task / 4 + 7) / 8 * 8, MAX_FANOUT));
            u32& fanout = fanouts[entry.pipeline];
            if (wanted > fanout) {
                if ((overflow && overflows_logged < 32) || learned_logged++ < 64) {
                    LOG_INFO(Render_Vulkan,
                             "Emulated mesh draws {:016x}: {} mesh workgroups per task workgroup "
                             "from now on",
                             entry.pipeline, wanted);
                }
                fanout = wanted;
            }
        }
        if (checking && ((overflow && overflows_logged++ < 32) || checks_logged++ < 64)) {
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

MeshEmulationBuffer::~MeshEmulationBuffer() {
    device.SetLossReporter({});
    scheduler.SetRenderPassEndHook({});
}

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
