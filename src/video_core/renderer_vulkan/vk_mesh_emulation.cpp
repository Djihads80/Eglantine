// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_mesh_emulation.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
using Shader::Backend::SPIRV::MeshEmulationArgs;
using Shader::Backend::SPIRV::MeshEmulationHeader;

constexpr u32 CHECK_SLOTS = 256;
constexpr VkDeviceSize CHECK_STRIDE = sizeof(MeshEmulationHeader) + sizeof(MeshEmulationArgs);
static_assert(MeshEmulationBuffer::CHECK_ARGS_OFFSET == sizeof(MeshEmulationHeader));
// Clamp pass descriptor sets: one per chunk.
constexpr u32 MAX_CHUNKS = 256;
// Mario Kart World's task workgroups launch up to ~6,000 mesh workgroups each.
constexpr u64 MAX_FANOUT = 16384;
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
    if (device.IsExtMeshShaderSupported()) {
        return false;
    }
    // Mario Kart World's task programs launch tens of thousands of mesh workgroups a draw:
    // emulated, a third of its frame rate on a phone, for distant trees not missed without.
    static constexpr u64 MARIO_KART_WORLD = 0x0400C3F00006E000ULL;
    return Settings::GetCurrentProgramID() != MARIO_KART_WORLD;
}

MeshEmulationBuffer::MeshEmulationBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                                         Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {
    const VkDeviceSize range = device.GetMaxStorageBufferRange();
#ifdef __ANDROID__
    // A phone's GPU memory is its RAM: a draw past this drops mesh workgroups instead.
    constexpr VkDeviceSize MAX_REGION = 32ULL << 20;
#else
    constexpr VkDeviceSize MAX_REGION = 256ULL << 20;
#endif
    max_size = std::min<VkDeviceSize>(range != 0 ? range : (128ULL << 20), MAX_REGION);
    alignment = std::max<VkDeviceSize>(device.GetStorageBufferAlignment(), 256);
    const vk::Device& logical = device.GetLogical();
    static constexpr VkDescriptorSetLayoutBinding CLAMP_BINDING{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    clamp_set_layout = logical.CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &CLAMP_BINDING,
    });
    static constexpr VkPushConstantRange CLAMP_CONSTANTS{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(Shader::Backend::SPIRV::MeshEmulationClampConstants),
    };
    clamp_layout = logical.CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = clamp_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &CLAMP_CONSTANTS,
    });
    clamp_module = BuildShader(device, Shader::Backend::SPIRV::EmitMeshEmulationClamp());
    clamp_pipeline = logical.CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *clamp_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *clamp_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });
    static constexpr VkDescriptorPoolSize CLAMP_POOL_SIZE{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .descriptorCount = MAX_CHUNKS,
    };
    clamp_pool = logical.CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_CHUNKS,
        .poolSizeCount = 1,
        .pPoolSizes = &CLAMP_POOL_SIZE,
    });
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
                .size = CHECK_SLOTS * CHECK_STRIDE,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }
}

std::pair<VkBuffer, VkDeviceSize> MeshEmulationBuffer::CheckSlot(u64 pipeline, u32 groups,
                                                                 bool has_task, u32 capacity,
                                                                 bool has_args) {
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
    checks.push_back(
        {scheduler.CurrentTick(), pipeline, slot, groups, capacity, has_task, has_args});
    return {*check_buffer, slot * CHECK_STRIDE};
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
        const u8* const data = check_buffer.Mapped().data() + entry.slot * CHECK_STRIDE;
        MeshEmulationHeader header;
        std::memcpy(&header, data, sizeof(header));
        MeshEmulationArgs args{};
        if (entry.has_args) {
            std::memcpy(&args, data + CHECK_ARGS_OFFSET, sizeof(args));
        }
        // The clamp pass found a header that was not the host's: nothing dispatched or drawn.
        const bool rejected = entry.has_args && args.rejected != 0;
        const bool overflow = entry.has_task && header.task_allocated > entry.capacity;
        // A header the draw's own work cannot have left (its dispatch bounded by its slots, its
        // capacity the host's): log it whole, learn nothing from it.
        const bool inconsistent =
            entry.has_task &&
            (header.dispatch[0] > entry.capacity || header.capacity != entry.capacity);
        if (inconsistent || rejected) {
            if (inconsistent_logged++ < 16) {
                const auto* const words = reinterpret_cast<const u32*>(&header);
                LOG_WARNING(Render_Vulkan,
                            "Emulated mesh draw {:016x}: inconsistent header after its work ({} "
                            "task workgroups, {} slots; {}): {:08x} {:08x} {:08x} {:08x} | "
                            "{:08x} {:08x} {:08x} {:08x} | {:08x} {:08x} {:08x} {:08x} {:08x} | "
                            "{:08x} {:08x} {:08x}",
                            entry.pipeline, entry.groups, entry.capacity,
                            !entry.has_args ? "no clamp pass"
                            : rejected      ? "the clamp pass dropped it"
                                            : "spoilt after the clamp pass",
                            words[0], words[1],
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

VkDescriptorSet MeshEmulationBuffer::MakeClampSet(VkBuffer buffer) {
    const VkDescriptorSetLayout layout = *clamp_set_layout;
    const vk::DescriptorSets sets = clamp_pool.Allocate(VkDescriptorSetAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *clamp_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    });
    if (sets.IsOutOfPoolMemory()) {
        LOG_WARNING(Render_Vulkan, "Task/mesh emulation: no clamp pass for the draws of chunk {}",
                    chunks.size());
        return VkDescriptorSet{};
    }
    // The args, then the header ArgsSpace() bytes further.
    const VkDescriptorBufferInfo info{
        .buffer = buffer,
        .offset = 0,
        .range = alignment + sizeof(MeshEmulationHeader),
    };
    device.GetLogical().UpdateDescriptorSets(
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = sets[0],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .pImageInfo = nullptr,
            .pBufferInfo = &info,
            .pTexelBufferView = nullptr,
        },
        {});
    return sets[0];
}

MeshEmulationRegion MeshEmulationBuffer::Allocate(VkDeviceSize request, bool args) {
    const VkDeviceSize space = args ? alignment : 0;
    const VkDeviceSize size = space + Common::AlignUp(request, alignment);
    const u64 tick = scheduler.CurrentTick();
    const auto take = [&](size_t index) {
        Chunk& chunk = chunks[index];
        const VkDeviceSize offset = chunk.used;
        chunk.used += size;
        chunk.tick = tick;
        current = index;
        return MeshEmulationRegion{
            .buffer = *chunk.buffer,
            .offset = offset + space,
            .clamp_set = args ? chunk.clamp_set : VkDescriptorSet{},
        };
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
    // Chunks long unused go before another is made (a few draws need tens of MiB).
    std::erase_if(chunks, [&](const Chunk& chunk) {
        if (!scheduler.IsFree(chunk.tick) || tick - chunk.tick <= 4096) {
            return false;
        }
        if (chunk.clamp_set) {
            VkDescriptorSet set = chunk.clamp_set;
            device.GetDispatchLoader().vkFreeDescriptorSets(*device.GetLogical(), *clamp_pool, 1,
                                                            &set);
        }
        return true;
    });
    const VkDeviceSize chunk_size = std::bit_ceil(std::max<VkDeviceSize>(size, 4ULL << 20));
    vk::Buffer buffer = memory_allocator.CreateBuffer(
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
        MemoryUsage::DeviceLocal);
    const VkDescriptorSet clamp_set = MakeClampSet(*buffer);
    chunks.push_back(Chunk{
        .buffer = std::move(buffer),
        .clamp_set = clamp_set,
        .size = chunk_size,
    });
    if (chunks_logged++ < 64) {
        LOG_INFO(Render_Vulkan, "Task/mesh emulation buffers: {}, the last of {} KiB",
                 chunks.size(), chunk_size / 1024);
    }
    return take(chunks.size() - 1);
}

} // namespace Vulkan
