// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <tuple>
#include <set>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "video_core/renderer_vulkan/renderer_vulkan.h"

#include "common/assert.h"
#include "shader_recompiler/exception.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/native_query_trace.h"
#include "video_core/indirect_reset.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/textures/texture.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_diag.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;


namespace {
/// read_gpu_memory: the first address space (the bound channel's first) that maps the whole range,
/// read as the GPU reads it, without flushing GPU caches.
VideoCore::Nx2Debug::GpuReadResult ReadGpuMemory(std::span<Tegra::MemoryManager* const> spaces,
                                                 GPUVAddr address, u64 size) {
    VideoCore::Nx2Debug::GpuReadResult result{};
    if (spaces.empty()) {
        result.error = "no GPU address space is bound yet";
        return result;
    }
    for (Tegra::MemoryManager* const space : spaces) {
        if (space->IsFullyMappedRange(address, size)) {
            result.bytes.resize(size);
            space->ReadBlockUnsafe(address, result.bytes.data(), size);
            result.address_space = space->GetID();
            return result;
        }
    }
    constexpr u64 page_mask = 0xfff;
    const GPUVAddr end = address + size;
    GPUVAddr first = address;
    while (first < end) {
        const GPUVAddr next = (std::min)((first | page_mask) + 1, end);
        if (!spaces.front()->IsFullyMappedRange(first, next - first)) {
            break;
        }
        first = next;
    }
    result.error = fmt::format("GPU address {:#x} is not mapped (the first unmapped byte in the "
                               "bound channel's address space; {} address spaces checked)",
                               first, spaces.size());
    return result;
}

struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z,
        .maxDepth = src.translate_z + src.scale_z,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor{};
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = (std::numeric_limits<s32>::max)();
        scissor.extent.height = (std::numeric_limits<s32>::max)();
    }
    return scissor;
}

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return false;
    default:
        return true;
    }
}

bool IsPrimitiveRestartSupported(const Device& device, VkPrimitiveTopology topology) {
    return ((topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsTopologyListPrimitiveRestartSupported()) ||
            SupportsPrimitiveRestart(topology) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsPatchListPrimitiveRestartSupported()));
}
} // Anonymous namespace

namespace {
// EDEN_NX2_DIAG_SAMPLES: the occlusion query pool of the draw probe (created on first use).
vk::QueryPool* g_diag_pool{};
} // Anonymous namespace
void RasterizerVulkan::ReadMeshEmulationProbe() {
    const MeshEmulationProbe probe{mesh_probe};
    mesh_probe.pending = false;
    const size_t num_words{probe.data.size() / sizeof(u32)};
    const auto word = [&](u64 index) -> u32 {
        if (index >= num_words) {
            return 0;
        }
        u32 value;
        std::memcpy(&value, probe.data.data() + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto position = [&](u64 base, u32 vertex) {
        std::array<f32, 4> result{};
        for (u32 element = 0; element < 4; ++element) {
            const u32 line{probe.position_lines[element]};
            if (line != ~0U) {
                result[element] = std::bit_cast<f32>(word(base + u64{vertex} * probe.vertex_lines +
                                                          line));
            }
        }
        return result;
    };
    u64 triangles{}, degenerate{}, outside{}, bad_index{};
    std::string sample;
    for (u32 group = 0; group < probe.groups; ++group) {
        const u64 base{u64{group} * probe.block_words};
        const u64 index_base{base + u64{probe.max_vertices} * probe.vertex_lines};
        for (u32 primitive = 0; primitive < probe.max_primitives; ++primitive) {
            const std::array<u32, 3> indices{word(index_base + primitive * 3),
                                             word(index_base + primitive * 3 + 1),
                                             word(index_base + primitive * 3 + 2)};
            if (indices[0] == indices[1] && indices[1] == indices[2]) {
                ++degenerate;
                continue;
            }
            ++triangles;
            bool any_bad{};
            bool any_inside{};
            for (const u32 vertex : indices) {
                if (vertex >= probe.max_vertices) {
                    any_bad = true;
                    continue;
                }
                const auto p{position(base, vertex)};
                const f32 w{std::abs(p[3])};
                any_inside |= w > 0.0f && std::abs(p[0]) <= w && std::abs(p[1]) <= w;
            }
            bad_index += any_bad;
            outside += !any_inside;
            if (sample.size() < 400 && triangles <= 2) {
                for (const u32 vertex : indices) {
                    const auto p{position(base, std::min(vertex, probe.max_vertices - 1))};
                    sample += fmt::format(" g{} v{} ({:.3g} {:.3g} {:.3g} {:.3g})", group, vertex,
                                          p[0], p[1], p[2], p[3]);
                }
            }
        }
    }
    LOG_INFO(Render_Vulkan,
             "Mesh emulation probe frame {}: {} groups, {} triangles ({} degenerate slots), {} "
             "with every vertex off screen, {} with a bad index; first triangles:{}",
             probe.frame, probe.groups, triangles, degenerate, outside, bad_index, sample);
}

const RasterizerVulkan::MeshArena& RasterizerVulkan::AcquireMeshArena() {
    const u64 tick{scheduler.CurrentTick()};
    if (mesh_arena_current < mesh_arenas.size() && mesh_arenas[mesh_arena_current].tick == tick) {
        return mesh_arenas[mesh_arena_current];
    }
    // A finished submission's counter holds the words its draws asked for, including those
    // that did not fit: grow later arenas to hold them.
    const u64 max_size{(std::min)(u64{1} << 30, u64{device.GetMaxStorageBufferRange()})};
    for (MeshArena& arena : mesh_arenas) {
        if (arena.counted || !scheduler.IsFree(arena.tick)) {
            continue;
        }
        arena.counted = true;
        arena.counter.Invalidate();
        u32 words;
        std::memcpy(&words, arena.counter.Mapped().data(), sizeof(words));
        const u64 needed{u64{words} * 4};
        if (needed > arena.size && mesh_arena_size < max_size) {
            mesh_arena_size = (std::min)(std::bit_ceil(needed + needed / 4), max_size);
            LOG_INFO(Render_Vulkan, "Mesh emulation: a submission needed {} MiB of output, arenas "
                                    "grow to {} MiB",
                     needed >> 20, mesh_arena_size >> 20);
        }
    }
    std::erase_if(mesh_arenas, [&](const MeshArena& arena) {
        return arena.counted && arena.size < mesh_arena_size;
    });
    auto it{std::ranges::find_if(mesh_arenas, [](const MeshArena& arena) { return arena.counted; })};
    if (it == mesh_arenas.end()) {
        const auto make_buffer = [&](u64 size, VkBufferUsageFlags usage, MemoryUsage memory) {
            return memory_allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = size,
                    .usage = usage,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                memory);
        };
        it = mesh_arenas.insert(
            mesh_arenas.end(),
            MeshArena{
                .buffer = make_buffer(mesh_arena_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      MemoryUsage::DeviceLocal),
                .counter = make_buffer(sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       MemoryUsage::Download),
                .size = mesh_arena_size,
            });
    }
    // The GPU is done with this arena, and this submission's commands have not been sent yet.
    const u32 zero{};
    std::memcpy(it->counter.Mapped().data(), &zero, sizeof(zero));
    it->counter.Flush();
    it->tick = tick;
    it->counted = false;
    mesh_arena_current = static_cast<size_t>(std::distance(mesh_arenas.begin(), it));
    return *it;
}

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE,
                             device.IsExtDescriptorBufferSupported()),
      compute_pass_descriptor_queue(device, UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE),
      descriptor_buffer_ring(device, memory_allocator),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool, texture_cache),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     descriptor_buffer_ring, render_pass_cache, buffer_cache, texture_cache,
                     gpu.ShaderNotify()),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler),
      frame_dump(device, memory_allocator, scheduler),
      wfi_event(device.GetLogical().CreateEvent()) {
    scheduler.SetQueryCache(query_cache);
    buffer_cache.SetPendingCpuWrites(
        [this](const std::function<void(DAddr, size_t)>& callback) {
            gpu.ForEachPendingCpuWrite(callback);
        });
    VideoCommon::GpuSplit::Get().SetIdleHook(
        [](void* argument) { static_cast<RasterizerVulkan*>(argument)->FlushSplitIdle(); },
        this);
    submit_while_waited =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::SubmitWhileWaited);
    if (submit_while_waited) {
        const auto draws = [](const char* name, u32 fallback) {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] < '0' || value[0] > '9') {
                return fallback;
            }
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            return *end == '\0' ? static_cast<u32>(std::clamp(parsed, 1UL, 1'048'576UL))
                                : fallback;
        };
        submit_waited_draws = draws("EDEN_NX2_SUBMIT_WAITED_DRAWS", 64);
        submit_idle_draws = draws("EDEN_NX2_SUBMIT_IDLE_DRAWS", 128);
        LOG_INFO(Render_Vulkan, "Nx2Perf submit_while_waited: {} draws while the front waits, {} "
                                "once the GPU is idle",
                 submit_waited_draws, submit_idle_draws);
    }
}

RasterizerVulkan::~RasterizerVulkan() {
    VideoCommon::GpuSplit::Get().SetIdleHook(nullptr, nullptr);
    // The channels are gone by now, and submitting invalidates the state tracker's flags.
    state_tracker.ClearChannel();
    scheduler.WaitWorker();
    scheduler.Finish();
    delete std::exchange(g_diag_pool, nullptr);
}

void RasterizerVulkan::Nx2ProveNonWriterDraw(const GraphicsPipeline& pipeline) {
    // The programs as RefreshStages resolved them, in the epoch read before it did: a program
    // dropped or rewritten since moved the epoch on, and the proof is stale from the start.
    const u64 epoch = pipeline_cache.Nx2ResolvedEpoch();
    const u64 map = pipeline_cache.Nx2ResolvedMap();
    if (epoch == 0 || map != gpu_memory->GetMapSerial()) {
        return;
    }
    Tegra::Nx2DrawPrograms programs;
    if (!Tegra::Engines::Maxwell3D::Nx2DrawProgramsOf(
            maxwell3d->regs, maxwell3d->GetNativeMacroEngine().native_turing, gpu_memory->GetID(),
            map, programs) ||
        Tegra::Nx2IsNonWriterDraw(programs, epoch)) {
        return;
    }
    // The indirect reset program is drawn by writing guest memory on the host.
    if (pipeline.IsMeshPipeline() || pipeline.Nx2MayWriteMemory() ||
        pipeline_cache.IsIndirectResetShader()) {
        return;
    }
    const auto& ranges = pipeline_cache.Nx2CodeRanges();
    for (const auto& [address, size] : ranges) {
        // A CPU write to GPU-modified buffer pages reaches the shader cache only later
        // (OnCPUWrite): code there could change under a proof without moving the epoch in time.
        if (size != 0 && buffer_cache.IsRegionGpuModified(address, size)) {
            return;
        }
    }
    for (const auto& [address, size] : ranges) {
        Tegra::Nx2MarkProvenCode(address, size);
    }
    // An invalidation on another thread meanwhile (CPU writes) makes the proof stale anyway.
    if (Tegra::g_nx2_code_epoch.load(std::memory_order_acquire) != epoch) {
        return;
    }
    Tegra::Nx2InsertNonWriterDraw(programs, epoch);
}

// Experimental adaptation of PokeDinner commit 76df4591a747: execute the reset shader's
// indirect record writes on the host until general GPU-address translation is available.
bool RasterizerVulkan::TryNx2IndirectReset() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESETFIX");
        return !value || std::string_view{value} != "0";
    }();
    if (!enabled || !maxwell3d->GetNativeMacroEngine().native_turing ||
        !pipeline_cache.IsIndirectResetShader()) {
        return false;
    }
    const auto reject = [](std::string_view reason) {
        static u32 reported_rejections{};
        if (reported_rejections++ < 8) {
            LOG_WARNING(HW_GPU, "Experimental indirect reset skipped: {}", reason);
        }
        return false;
    };
    const auto in_gpu_range = [this](u64 address, size_t size) {
        return size != 0 && address <= (std::numeric_limits<u64>::max)() - size &&
               gpu_memory->IsWithinGPUAddressRange(address) &&
               gpu_memory->IsWithinGPUAddressRange(address + size - 1);
    };
    const auto& cbuf = maxwell3d->state.shader_stages[0].const_buffers[0];
    if (!cbuf.enabled || cbuf.size < 0x120 || !in_gpu_range(cbuf.address, 0x120) ||
        !gpu_memory->IsFullyMappedRange(cbuf.address + 0x110, 16)) {
        return reject("descriptor unavailable");
    }
    std::array<u64, 2> descriptor{};
    gpu_memory->ReadBlock(cbuf.address + 0x110, descriptor.data(), sizeof(descriptor));
    const u64 list = descriptor[0];
    const auto& draw = maxwell3d->draw_manager.draw_state;
    const u64 first = draw.vertex_buffer.first;
    const u64 count = draw.vertex_buffer.count;
    const auto range = VideoCommon::IndirectResetRange(list, static_cast<u32>(descriptor[1]),
                                                        first, count);
    if (!range || !in_gpu_range(range->begin, range->size) ||
        !gpu_memory->IsFullyMappedRange(range->begin, range->size)) {
        return reject("list outside mapped descriptor");
    }
    if (gpu_memory->IsMemoryDirty(range->begin, range->size, VideoCommon::CacheType::BufferCache)) {
        gpu_memory->FlushRegion(range->begin, range->size, VideoCommon::CacheType::BufferCache);
    }
    std::vector<u64> pointers(count);
    gpu_memory->ReadBlockUnsafe(range->begin, pointers.data(), range->size);
    static constexpr std::array<u32, 5> record{0, 1, 0, 0, 0};
    boost::container::small_vector<VideoCommon::Nx2ResetRun, 16> runs;

    for (const u64 pointer : pointers) {
        if (!pointer) {
            continue;
        }
        if (!in_gpu_range(pointer, sizeof(record)) ||
            !gpu_memory->IsFullyMappedRange(pointer, sizeof(record))) {
            return reject("record not mapped");
        }
        if (pointer < range->begin + range->size && range->begin < pointer + sizeof(record)) {
            return reject("record overlaps pointer list");
        }
        const auto segments = gpu_memory->GetSubmappedRange(pointer, sizeof(record));
        if (segments.size() != 1 || segments[0].first != pointer ||
            segments[0].second != sizeof(record)) {
            return reject("record crosses a mapping boundary");
        }
        const auto cpu_addr = gpu_memory->GpuToCpuAddress(pointer);
        if (!cpu_addr) {
            return reject("record not mapped to CPU address");
        }
        if (!runs.empty()) {
            auto& last = runs.back();
            const u64 expected_gpu = last.gpu_addr + last.count * sizeof(record);
            const u64 expected_cpu = last.cpu_addr + last.count * sizeof(record);
            if (pointer == expected_gpu && *cpu_addr == expected_cpu) {
                last.count++;
                continue;
            }
        }
        runs.push_back(VideoCommon::Nx2ResetRun{
            .gpu_addr = pointer,
            .cpu_addr = *cpu_addr,
            .count = 1,
        });
    }

    if (runs.empty()) {
        return true;
    }

    // Prepare repeating pattern buffer
    size_t max_records = 0;
    for (const auto& run : runs) {
        max_records = std::max<size_t>(max_records, static_cast<size_t>(run.count));
    }
    std::vector<u32> pattern(max_records * 5);
    for (size_t i = 0; i < max_records; ++i) {
        std::memcpy(pattern.data() + i * 5, record.data(), sizeof(record));
    }
    const u8* const pattern_bytes = reinterpret_cast<const u8*>(pattern.data());

    // 1. Batched guest-memory writes
    for (const auto& run : runs) {
        const size_t run_bytes = run.count * sizeof(record);
        gpu_memory->WriteBlockUnsafe(run.gpu_addr, pattern_bytes, run_bytes);
    }

    static const bool defer = [] {
        const char* value = std::getenv("EDEN_NX2_INLINE_DEFER");
        return value == nullptr || value[0] != '0';
    }();

    // 2. Batched buffer cache upload under a single lock
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (auto& run : runs) {
            const size_t run_bytes = run.count * sizeof(record);
            buffer_cache.Nx2NoteListHostWrite(run.cpu_addr, run_bytes,
                                              std::span{pattern_bytes, run_bytes});
            run.uploaded =
                buffer_cache.InlineMemory(run.cpu_addr, run_bytes, std::span{pattern_bytes, run_bytes});
            if (!run.uploaded && !defer) {
                buffer_cache.WriteMemory(run.cpu_addr, run_bytes);
            }
        }
    }

    // 3. Invalidation passes over coalesced runs (matching AccelerateInlineToMemory)
    bool any_immediate = false;
    for (const auto& run : runs) {
        const size_t run_bytes = run.count * sizeof(record);
        query_cache.InvalidateRegion(run.cpu_addr, run_bytes);
        if (!run.uploaded && defer) {
            gpu_memory->AccumulateInvalidation(run.gpu_addr, run_bytes);
        } else {
            any_immediate = true;
        }
    }
    if (any_immediate) {
        {
            std::scoped_lock lock{texture_cache.mutex};
            for (const auto& run : runs) {
                if (run.uploaded || !defer) {
                    texture_cache.WriteMemory(run.cpu_addr, run.count * sizeof(record));
                }
            }
        }
        for (const auto& run : runs) {
            if (run.uploaded || !defer) {
                pipeline_cache.InvalidateRegion(run.cpu_addr, run.count * sizeof(record));
            }
        }
    }
    static u32 reported{};
    if (reported++ < 5) {
        LOG_INFO(HW_GPU, "Experimental indirect reset: {} records", count);
    }
    return true;
}

namespace {
// EDEN_NX2_DIAG*: what the draw being prepared is (set by Draw/DrawIndirect, read by PrepareDraw).
struct DiagDraw {
    const char* kind{"draw"};
    u32 vertices{};
    u32 instances{};
};
thread_local DiagDraw g_diag_draw;
thread_local int g_diag_query{-1};

u64 DiagShader(const Tegra::Engines::Maxwell3D& maxwell3d, size_t stage) {
    const auto& regs = maxwell3d.regs;
    return (u64{regs.pipelines[stage].reserved[0] & 0xff} << 32) | regs.pipelines[stage].reserved[1];
}

// diag_draw_lite (EDEN_NX2_DIAG only): the back's draw bookkeeping without a lock per draw.
bool DiagLite() {
    static const bool on = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::DiagDrawLite);
    return on;
}

// Verify only, back thread: differences found and the error lines left to log.
u64 g_lite_differences{};
u32 g_lite_lines{64};

VideoCommon::Nx2Diag::LiteTable& DiagLiteTable() {
    auto& table = VideoCommon::Nx2Diag::ThreadLite();
    if (!table) [[unlikely]] {
        table = std::make_unique<VideoCommon::Nx2Diag::LiteTable>();
        VideoCommon::Nx2Diag::Diagnostics::Get().EnableLite();
    }
    return *table;
}

// The cached answers for a draw's programs. Verify: the synchronous answers, compared with the
// cached ones when no generation change came between.
VideoCommon::Nx2Diag::LiteAnswer DiagLiteAnswer(VideoCommon::Nx2Diag::Diagnostics& diag,
                                                VideoCommon::Nx2Diag::LiteTable& table, u64 vs,
                                                u64 ps) {
    const auto& cached = diag.LiteLookup(table, vs, ps);
    if (!VideoCommon::Nx2Perf::Verify()) {
        return cached;
    }
    // Polled like the synchronous Ablated/Inspecting calls.
    const auto fresh = diag.Resolve(vs, ps, true);
    if (fresh.generation == cached.generation &&
        (fresh.vkey != cached.vkey || fresh.pkey != cached.pkey ||
         fresh.ablated_v != cached.ablated_v || fresh.ablated_p != cached.ablated_p ||
         fresh.inspect_v != cached.inspect_v || fresh.inspect_p != cached.inspect_p)) {
        ++g_lite_differences;
        if (g_lite_lines > 0) {
            --g_lite_lines;
            LOG_ERROR(HW_GPU, "Nx2Perf diag_draw_lite: answer differs for vs={:#x} ps={:#x}", vs, ps);
        }
    }
    return fresh;
}

// Verify, after the present's fold: the folded state against the shadow's synchronous one.
void DiagLiteVerifyFrame() {
    auto& diag = VideoCommon::Nx2Diag::Diagnostics::Get();
    if (const auto& table = VideoCommon::Nx2Diag::ThreadLite(); table && table->used != 0) {
        diag.FoldLite(*table);
    }
    g_lite_differences += diag.DrawDifferences(VideoCommon::Nx2Diag::Diagnostics::Shadow(), g_lite_lines);
    static std::chrono::steady_clock::time_point last_log{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds{1}) {
        last_log = now;
        if (g_lite_differences != 0) {
            LOG_ERROR(HW_GPU, "Nx2Perf diag_draw_lite: {} differences", g_lite_differences);
        } else {
            LOG_INFO(HW_GPU, "Nx2Perf diag_draw_lite: 0 differences");
        }
    }
}

bool DiagAblated(const Tegra::Engines::Maxwell3D& maxwell3d) {
    auto& diag = VideoCommon::Nx2Diag::Diagnostics::Get();
    if (DiagLite()) {
        const auto answer = DiagLiteAnswer(diag, DiagLiteTable(), DiagShader(maxwell3d, 1),
                                           DiagShader(maxwell3d, 5));
        return answer.ablated_v || answer.ablated_p;
    }
    return diag.Ablated('v', DiagShader(maxwell3d, 1)) || diag.Ablated('p', DiagShader(maxwell3d, 5));
}

// Shows the constant buffers bound to a stage to the inspector (inspect.txt).
void DiagObserveStage(Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& gpu_memory,
                      size_t stage, u64 key) {
    auto& diag = VideoCommon::Nx2Diag::Diagnostics::Get();
    const auto& info = maxwell3d.state.shader_stages[stage];
    for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
        const auto& buffer = info.const_buffers[slot];
        if (!buffer.enabled || buffer.size == 0) {
            continue;
        }
        std::vector<u32> words(std::min<u32>(buffer.size, 0x2000) / 4);
        if (words.empty() || !gpu_memory.IsFullyMappedRange(buffer.address, words.size() * 4)) {
            continue;
        }
        gpu_memory.ReadBlockUnsafe(buffer.address, words.data(), words.size() * 4);
        diag.Observe(key, slot, words);
    }
}

void DiagRecordDraw(Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& gpu_memory,
                    const char* outcome, bool mesh) {
    auto& diag = VideoCommon::Nx2Diag::Diagnostics::Get();
    const u64 vs = DiagShader(maxwell3d, 1);
    const u64 ps = DiagShader(maxwell3d, 5);
    if (DiagLite()) {
        auto& table = DiagLiteTable();
        const auto answer = DiagLiteAnswer(diag, table, vs, ps);
        const auto& rt = maxwell3d.regs.rt[0];
        if (VideoCommon::Nx2Perf::Verify()) {
            VideoCommon::Nx2Diag::Diagnostics::Shadow().ShadowDraw(
                diag.CurrentFrame(), answer, outcome, g_diag_draw.instances, g_diag_draw.vertices,
                rt.width, rt.height, VideoCommon::Nx2Diag::CountersEnabled());
        }
        if (!diag.Capturing()) {
            diag.LiteAdd(table, answer, outcome, g_diag_draw.instances, g_diag_draw.vertices,
                         rt.width, rt.height);
            if (answer.inspect_v) {
                DiagObserveStage(maxwell3d, gpu_memory, 0, answer.vkey);
            }
            if (answer.inspect_p) {
                DiagObserveStage(maxwell3d, gpu_memory, 4, answer.pkey);
            }
            g_diag_query = -1;
            return;
        }
        // A capture: the synchronous path, after this thread's earlier draws.
        if (table.used != 0) {
            diag.FoldLite(table);
        }
    }
    {
        const auto& rt = maxwell3d.regs.rt[0];
        diag.Touch('v', vs, outcome, g_diag_draw.instances, g_diag_draw.vertices, rt.width, rt.height);
        diag.Touch('p', ps, outcome, g_diag_draw.instances, g_diag_draw.vertices, rt.width, rt.height);
        if (const u64 key = diag.ProgramKey(vs); diag.Inspecting(key)) {
            DiagObserveStage(maxwell3d, gpu_memory, 0, key);
        }
        if (const u64 key = diag.ProgramKey(ps); diag.Inspecting(key)) {
            DiagObserveStage(maxwell3d, gpu_memory, 4, key);
        }
    }
    if (VideoCommon::Nx2Diag::CountersEnabled()) {
        diag.Note(false, vs, ps, outcome, g_diag_draw.instances, g_diag_draw.vertices);
    }
    if (!diag.Capturing()) {
        g_diag_query = -1;
    } else {
        const auto& regs = maxwell3d.regs;
        diag.Row(fmt::format(
            "{}\t{}\t{:#x}\t{:#x}\t{}\t{}\t{}\t{}\t{:#x}\t{}\t{}\t{:#x}\t{}\t{}\t{:#x}\t{}\t{:016x}\t{:016x}",
            diag.NextSeq(), g_diag_draw.kind, vs, ps, outcome, g_diag_draw.vertices,
            g_diag_draw.instances, static_cast<u32>(maxwell3d.draw_manager.draw_state.topology),
            regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
            static_cast<u32>(regs.rt[0].format), regs.zeta_enable, regs.blend.enable[0],
            regs.color_mask[0].raw, mesh ? "mesh" : "", diag.ProgramKey(vs), diag.ProgramKey(ps)),
            std::exchange(g_diag_query, -1), vs, ps);
    }
}
} // namespace

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    bool submitted = false;
    SCOPE_EXIT {
        auto& native = maxwell3d->GetNativeMacroEngine();
        if (native.native_turing && !submitted) {
            native.native_execution_failed = true;
            LOG_DEBUG(HW_GPU, "Native graphics draw failed; refusing submission completion");
        }
    };
    namespace Profile = VideoCommon::Nx2Profile;
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
        gpu_memory->FlushCaching();
    }
    if (frame_dump.Capturing()) {
        frame_dump.DrainIfFull();
    }

    const char* diag_outcome = "drawn";
    const auto report_failure = [&](std::string_view reason) {
        diag_outcome = reason == "no pipeline" ? pipeline_cache.GraphicsMissReason()
                       : reason == "configure failed" ? "configure-failed" : "mesh-disabled";
        static std::set<std::tuple<u64, u64, std::string_view>> seen;
        static size_t async_logged{};
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        // Say why, and keep the expected asynchronous-shader skips from using up the line budget.
        const std::string_view miss = reason == "no pipeline" ? diag_outcome : reason;
        const bool async_miss = miss == "async-build";
        if ((async_miss ? async_logged < 8 : seen.size() < 64) && seen.emplace(vs, ps, miss).second) {
            async_logged += async_miss ? 1 : 0;
            LOG_WARNING(HW_GPU, "Draw skipped ({}): vs={:#x} ps={:#x} rt0={:#x} {}x{} miss={}", reason, vs,
                        ps, regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height, miss);
        }
    };
    const bool diag_active = VideoCommon::Nx2Diag::Active();
    SCOPE_EXIT {
        if (diag_active) {
            DiagRecordDraw(*maxwell3d, *gpu_memory, diag_outcome, mesh_draw);
        }
    };
    GraphicsPipeline* const pipeline{[&] {
        Profile::Scope scope{Profile::Zone::PipelineLookup};
        return pipeline_cache.CurrentGraphicsPipeline(is_indexed);
    }()};
    if (!pipeline) {
        report_failure("no pipeline");
        return;
    }
    if (!is_indexed && TryNx2IndirectReset()) {
        submitted = true;
        return;
    }
    // Task/mesh pipelines: the draw's vertex count is the number of task workgroups.
    mesh_draw = pipeline->IsMeshPipeline();
    // EDEN_NX2_NO_MESH=1: skip task/mesh draws as before they were supported (A/B comparisons).
    // EDEN_NX2_NO_MESH_FILE=<path>: skip them while that file exists (checked every 1024 draws),
    // to compare the same scene with and without them in one run.
    static const bool no_mesh_always{std::getenv("EDEN_NX2_NO_MESH") != nullptr};
    static const char* const no_mesh_file{std::getenv("EDEN_NX2_NO_MESH_FILE")};
    static bool no_mesh{no_mesh_always};
    static u32 no_mesh_check{};
    if (no_mesh_file && (no_mesh_check++ % 1024) == 0) {
        no_mesh = std::filesystem::exists(no_mesh_file);
    }
    const bool emulated_mesh{mesh_draw && pipeline->IsEmulatedMesh()};
    // Diagnostics: a frame stamp for the emulation's statistics and probes.
    static u64 render_trace_frame{};
    ++render_trace_frame;
    if (mesh_draw && (no_mesh || (!device.IsExtMeshShaderSupported() && !emulated_mesh))) {
        report_failure(no_mesh ? "EDEN_NX2_NO_MESH" : "no VK_EXT_mesh_shader");
        return;
    }
    // Mesh emulation: one output block per mesh workgroup, written by the pipeline's compute
    // dispatch and drawn as groups * vertices-per-group triangle list vertices.
    mesh_emulation_groups = 0;
    mesh_emulation_vertices_per_group = 0;
    if (emulated_mesh) {
        // The same group count the native path hands vkCmdDrawMeshTasksEXT (MakeDrawParams:
        // the index count for indexed draws), within the device's dispatch limit.
        const auto& draw_state{maxwell3d->draw_manager.draw_state};
        const u32 groups{(std::min)(MakeDrawParams(draw_state, 1, is_indexed).num_vertices,
                                    device.GetMaxComputeWorkGroupCount()[0])};
        // Mesh emulation statistics: draws with and without groups, and the frames with any
        // groups (the game culls on the GPU and sets the counts itself).
        {
            static u64 draws{}, empty{}, total_groups{}, frames{}, frames_drawn{};
            static u64 last_frame{~0ULL};
            static bool frame_drawn{};
            if (render_trace_frame != last_frame) {
                if (last_frame != ~0ULL) {
                    ++frames;
                    frames_drawn += frame_drawn;
                }
                last_frame = render_trace_frame;
                frame_drawn = false;
            }
            ++draws;
            empty += groups == 0;
            total_groups += groups;
            frame_drawn |= groups != 0;
            if (draws % 2000 == 0) {
                LOG_INFO(Render_Vulkan,
                         "Mesh emulation draws: {} ({} with no groups), {} groups; frames with "
                         "emulated draws: {} of {}",
                         draws, empty, total_groups, frames_drawn, frames);
            }
        }
        // Read the pending probe before Request can hand its buffer out again.
        if (mesh_probe.pending && scheduler.IsFree(mesh_probe.tick)) {
            ReadMeshEmulationProbe();
        }
        // EDEN_NX2_MESH_EMULATION_PROBE=1: every 60 frames, read one draw's output back (from a
        // host-visible download buffer, as device-local ones are not mapped on every GPU).
        static const bool probe{std::getenv("EDEN_NX2_MESH_EMULATION_PROBE") != nullptr};
        const bool has_task{pipeline->MeshEmulationHasTask()};
        const bool probe_draw{probe && !has_task && !mesh_probe.pending && groups != 0 &&
                              render_trace_frame % 60 == 0};
        // With a task program the draw's groups are task workgroups. The mesh workgroups they ask
        // for are only known on the GPU, so their output goes to the submission's arena.
        const u64 block_bytes{pipeline->MeshEmulationBlockBytes()};
        StagingBufferRef output{};
        u64 bytes{};
        if (has_task) {
            const MeshArena& arena{AcquireMeshArena()};
            bytes = arena.size;
            pipeline->SetMeshEmulationOutput(*arena.buffer, 0, bytes, groups);
        } else {
            bytes = (std::max)(u64{groups} * block_bytes, u64{4});
            output = staging_pool.Request(bytes, probe_draw ? MemoryUsage::Download
                                                            : MemoryUsage::DeviceLocal);
            pipeline->SetMeshEmulationOutput(output.buffer, output.offset, bytes, groups);
        }
        mesh_emulation_indirect = {};
        if (mesh_task_probe.pending && scheduler.IsFree(mesh_task_probe.tick)) {
            mesh_task_probe.pending = false;
            std::array<u32, Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS> words{};
            std::memcpy(words.data(), mesh_task_probe.args.data(), sizeof(words));
            LOG_INFO(Render_Vulkan,
                     "Mesh emulation task probe: {} tasks asked for {} mesh workgroups, {} "
                     "dispatched at arena word {} (arena {} words)",
                     mesh_task_probe.tasks,
                     words[Shader::Backend::SPIRV::MESH_EMULATION_ARGS_REQUESTED], words[0],
                     words[Shader::Backend::SPIRV::MESH_EMULATION_ARGS_BASE],
                     mesh_task_probe.capacity);
        }
        if (has_task && groups != 0) {
            const bool task_probe{probe && !mesh_task_probe.pending &&
                                  render_trace_frame % 60 == 0};
            const auto request = [&](u64 size, MemoryUsage usage = MemoryUsage::DeviceLocal) {
                size = (std::max)(size, u64{16});
                const StagingBufferRef ref{staging_pool.Request(size, usage)};
                return std::make_pair(GraphicsPipeline::MeshEmulationBuffer{ref.buffer, ref.offset,
                                                                            size},
                                      ref.mapped_span.subspan(
                                          (std::min)(ref.offset, u64{ref.mapped_span.size()})));
            };
            const auto payload{
                request(u64{groups} * pipeline->MeshEmulationPayloadStride() * 4).first};
            const auto offsets{request(u64{groups} * 4).first};
            constexpr u64 args_bytes{Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS * 4};
            const auto [args, args_span] =
                request(args_bytes, task_probe ? MemoryUsage::Download : MemoryUsage::DeviceLocal);
            const u32 capacity_words{static_cast<u32>(
                (std::min)(bytes / 4, u64{(std::numeric_limits<u32>::max)()}))};
            if (task_probe && args_span.size() >= args_bytes) {
                mesh_task_probe = {
                    .args = args_span,
                    .tick = scheduler.CurrentTick(),
                    .tasks = groups,
                    .capacity = capacity_words,
                    .pending = true,
                };
            }
            const MeshArena& arena{mesh_arenas[mesh_arena_current]};
            pipeline->SetMeshEmulationTaskBuffers(
                payload, offsets, args, {*arena.counter, 0, sizeof(u32)}, capacity_words);
            mesh_emulation_indirect = {args.buffer, args.offset};
        }
        if (probe_draw && output.mapped_span.size() >= output.offset + bytes) {
            const MeshEmulationSetup& setup{*pipeline->MeshEmulation()};
            mesh_probe = {
                .data = output.mapped_span.subspan(output.offset, bytes),
                .tick = scheduler.CurrentTick(),
                .frame = render_trace_frame,
                .groups = groups,
                .block_words = setup.block_words,
                .vertex_lines = setup.vertex_lines,
                .max_vertices = setup.max_vertices,
                .max_primitives = setup.max_primitives,
                .position_lines = setup.position_lines,
                .pending = true,
            };
        } else if (probe_draw && output.mapped_span.empty()) {
            static bool warned{};
            if (!warned) {
                warned = true;
                LOG_WARNING(Render_Vulkan, "Mesh emulation probe: output buffer is not mapped");
            }
        }
        mesh_emulation_groups = groups;
        mesh_emulation_vertices_per_group = pipeline->MeshEmulationVerticesPerGroup();
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadWriterSet)) {
        Nx2ProveNonWriterDraw(*pipeline);
    }
    if (buffer_cache.HasPendingNx2ListCopies() &&
        !Tegra::Engines::Maxwell3D::FrontIsWriterDraw(maxwell3d->regs)) {
        SubmitNx2ListCopies();
    }
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    const u32 repack_dirty = buffer_cache_runtime.SetVertexRepackLayout(
        maxwell3d->regs, pipeline->HasDynamicVertexInput());
    if (repack_dirty) {
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        flags[Dirty::VertexInput] = true;
        for (u32 binding = 0; binding < 32; ++binding) {
            if (repack_dirty & (1U << binding)) {
                flags[VideoCommon::Dirty::VertexBuffer0 + binding] = true;
            }
        }
    }
    // Enable command list copy collection around pipeline configure and draw
    buffer_cache.Nx2CollectListCopies(true);
    SCOPE_EXIT {
        buffer_cache.Nx2CollectListCopies(false);
    };

    if (Profile::Scope scope{Profile::Zone::Configure}; !pipeline->Configure(is_indexed)) {
        report_failure("configure failed");
        return;
    }
    if (frame_dump.Capturing()) {
        frame_dump.NoteKeys(pipeline->ShaderHashes(),
                            pipeline_cache.GraphicsNativeKeys(pipeline->ShaderHashes()));
        FrameDumpPass(texture_cache.GetFramebuffer());
        frame_dump.NoteDraw(pipeline->ShaderHashes());
        bool recorded = false;
        if (frame_dump.CapturingSampled()) {
            boost::container::small_vector<FrameDump::Sampled, 16> sampled;
            for (const auto& image : pipeline->SampledImages()) {
                sampled.push_back({image.stage, image.tic, &texture_cache.GetImageView(image.id),
                                   &texture_cache.GetSampler(image.sampler)});
            }
            recorded = frame_dump.NoteSampled(sampled);
        }
        if (frame_dump.CapturingBuffers()) {
            recorded |= FrameDumpBuffers(false, pipeline->ShaderHashes());
        }
        if (recorded) {
            scheduler.RequestRenderpass(texture_cache.GetFramebuffer());
        }
    }

    NoteCameraMotionDraw();
    {
        Profile::Scope scope{Profile::Zone::DynamicState};
        UpdateDynamicStates(pipeline);
    }

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    // EDEN_NX2_DIAG_SAMPLES: count the samples this draw passes while a draw list is captured. The
    // game's own occlusion counting is left alone (one occlusion query can be active at a time).
    int diag_query = -1;
    if (diag_active && VideoCommon::Nx2Diag::SamplesEnabled() && g_diag_pool != nullptr &&
        !maxwell3d->regs.zpass_pixel_count_enable) {
        diag_query = VideoCommon::Nx2Diag::Diagnostics::Get().NextQuery();
    }
    if (diag_query >= 0) {
        scheduler.Record([pool = **g_diag_pool, diag_query](vk::CommandBuffer cmdbuf) {
            cmdbuf.BeginQuery(pool, static_cast<u32>(diag_query), 0);
        });
    }
    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
        draw_func();
    }
    if (diag_query >= 0) {
        scheduler.Record([pool = **g_diag_pool, diag_query](vk::CommandBuffer cmdbuf) {
            cmdbuf.EndQuery(pool, static_cast<u32>(diag_query));
        });
        g_diag_query = diag_query;
    }
    submitted = true;
    static u32 native_capture_count{};
    if (maxwell3d->GetNativeMacroEngine().native_turing &&
        native_capture_count < 16 && [] {
            static const bool capture = std::getenv("EDEN_NATIVE_DRAW_CAPTURE") != nullptr;
            return capture;
        }()) {
        const u32 capture = native_capture_count++;
        const auto prefix = fmt::format("native-draw-{}", capture);
        const auto dump = [&](const std::string& name, GPUVAddr address, size_t size) {
            if (!size || !gpu_memory->IsFullyMappedRange(address, size)) return;
            std::vector<u8> bytes(size);
            gpu_memory->ReadBlockUnsafe(address, bytes.data(), bytes.size());
            std::ofstream file(prefix + name + ".bin", std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            LOG_INFO(HW_GPU, "Native draw capture {}{}: address={:#x}, bytes={}, nonzero={}",
                     capture, name, address, size,
                     std::count_if(bytes.begin(), bytes.end(), [](u8 byte) { return byte != 0; }));
        };
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const size_t size = static_cast<size_t>(rt.array_pitch) * 4;
        if (size && size <= 16 * 1024 * 1024) {
            if (const auto address = gpu_memory->GpuToCpuAddress(rt.Address(), size)) {
                LOG_INFO(HW_GPU, "Native draw {} render target physical address={:#x}", capture, *address);
                texture_cache.DownloadMemory(*address, size);
                dump("-rt0", rt.Address(), size);
            }
        }
        std::ofstream registers(prefix + "-registers.bin", std::ios::binary);
        registers.write(reinterpret_cast<const char*>(&regs), sizeof(regs));
        for (size_t stream = 0; stream < 4; ++stream) {
            if (regs.vertex_streams[stream].IsEnabled())
                dump(fmt::format("-vertex-{}", stream), regs.vertex_streams[stream].Address(), 128);
        }
        for (size_t stage : {0U, 4U}) {
            for (size_t bank : {2U, 3U}) {
                const auto& cb = maxwell3d->state.shader_stages[stage].const_buffers[bank];
                if (cb.enabled) dump(fmt::format("-cbuf-{}-{}", stage, bank), cb.address, std::min<size_t>(cb.size, 128));
            }
        }
    }
}


namespace {
// EDEN_NX2_WINDOW_TRACE: describe clears and draws into screen-sized colour targets (debug aid).
void TraceWindowTarget(const Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& memory,
                       const char* kind, u32 count) {
    static const bool trace = std::getenv("EDEN_NX2_WINDOW_TRACE") != nullptr;
    if (!trace) [[likely]] {
        return;
    }
    static u32 traced{};
    const auto& regs = maxwell3d.regs;
    static const u64 skip = [] {
        const char* value = std::getenv("EDEN_NX2_WINDOW_TRACE_SKIP");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    static u64 seen_events{};
    static const u64 only_ps = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_PS");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    const u64 pixel_shader =
        (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
    if (regs.rt[0].width != 3840 || traced >= 400 || seen_events++ < skip ||
        (only_ps != 0 && pixel_shader != only_ps)) {
        return;
    }
    ++traced;
    const auto& vp = regs.viewport_transform[0];
    const auto& sc = regs.scissor_test[0];
    LOG_INFO(HW_GPU,
             "nx2 window {} rt0={:#x} count={} clear={:#x} color=({},{},{},{}) mask_common={} "
             "mask0={:#x} rt_count={} vp scale=({},{}) translate=({},{}) scissor en={} x={}..{} "
             "y={}..{} blend0={} vs={:#x} ps={:#x} rasterize={}",
             kind, regs.rt[0].Address(), count, regs.clear_surface.raw, regs.clear_color[0],
             regs.clear_color[1], regs.clear_color[2], regs.clear_color[3], regs.color_mask_common,
             regs.color_mask[0].raw, regs.rt_control.count.Value(), vp.scale_x, vp.scale_y,
             vp.translate_x, vp.translate_y, sc.enable, sc.min_x.Value(), sc.max_x.Value(),
             sc.min_y.Value(), sc.max_y.Value(), regs.blend.enable[0],
             (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1],
             (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1],
             regs.rasterize_enable);
    if (kind[0] != 'd') {
        return;
    }
    // Pixel-stage constant buffer 3: colour constants of the loading-screen passes.
    if (const auto& cb3 = maxwell3d.state.shader_stages[4].const_buffers[3]; cb3.enabled) {
        std::array<f32, 8> values{};
        memory.ReadBlockUnsafe(cb3.address, values.data(), sizeof(values));
        LOG_INFO(HW_GPU, "nx2 window ps c[3]@{:#x} size={}: {}", cb3.address, cb3.size,
                 fmt::join(values, " "));
    }
    static u32 texture_traces{};
    if (texture_traces++ > 200) {
        return;
    }
    // Texture used by the pixel shader's first bound TEX (handle in c[2][0x20]) and its TIC.
    const auto& cb = maxwell3d.state.shader_stages[4].const_buffers[2];
    const u32 handle = cb.enabled ? memory.Read<u32>(cb.address + 0x20) : 0;
    const u32 tic_index = handle & 0xfffff;
    const GPUVAddr tic_address = regs.tex_header.Address() + u64{tic_index} * 32;
    std::array<u32, 8> tic{};
    memory.ReadBlockUnsafe(tic_address, tic.data(), sizeof(tic));
    LOG_INFO(HW_GPU,
             "nx2 window texture: cb2={:#x} en={} handle={:#x} tic pool={:#x} limit={} "
             "sampler pool={:#x} tic[{}]={:08x}",
             cb.address, cb.enabled, handle, regs.tex_header.Address(), regs.tex_header.limit,
             regs.tex_sampler.Address(), tic_index, fmt::join(tic, " "));
    // First bytes of the texture's memory (block-linear, so just a sample).
    const GPUVAddr texture = (GPUVAddr{tic[2] & 0xffff} << 32) | tic[1];
    std::array<u32, 16> texels{};
    memory.ReadBlockUnsafe(texture, texels.data(), sizeof(texels));
    LOG_INFO(HW_GPU, "nx2 window texture {:#x} {}x{}: {:08x}", texture, (tic[4] & 0xffff) + 1,
             (tic[5] & 0xffff) + 1, fmt::join(texels, " "));
    // EDEN_NX2_TEXTURE_DUMP=<file>: save the first 4 MiB of the last traced texture's memory.
    if (const char* path = std::getenv("EDEN_NX2_TEXTURE_DUMP"); path != nullptr) {
        std::vector<u8> bytes(2048u * 10240u);
        memory.ReadBlockUnsafe(texture, bytes.data(), bytes.size());
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }
}
} // Anonymous namespace

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    {
        // A draw's first and count can come from memory a skipped (rejected) compute program
        // should have written. Past 16M vertices they are garbage: building the quad index
        // buffer for one threw out of the GPU thread and ended the process (Hyrule Warriors:
        // Age of Imprisonment, entering a new area), and indexed ones read far out of bounds.
        const auto& state = maxwell3d->draw_manager.draw_state;
        const u64 first = is_indexed ? state.index_buffer.first : state.vertex_buffer.first;
        const u64 count = is_indexed ? state.index_buffer.count : state.vertex_buffer.count;
        constexpr u64 MaxVertices = u64{1} << 24;
        if (first + count > MaxVertices) {
            static std::atomic<u32> skipped{};
            if (skipped.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(HW_GPU, "Draw skipped: {} {} from {} (vertex/index count past {})",
                            count, is_indexed ? "indices" : "vertices", first, MaxVertices);
            }
            return;
        }
    }
    TraceWindowTarget(*maxwell3d, *gpu_memory, is_indexed ? "draw-indexed" : "draw",
                      is_indexed ? maxwell3d->draw_manager.draw_state.index_buffer.count
                                 : maxwell3d->draw_manager.draw_state.vertex_buffer.count);
    // EDEN_NX2_CB24_TRACE: for draws whose vertex shader reads bank 24, show the inline
    // constant buffer upload target (CB_ADDRESS) and its first words.
    if (static const bool trace = std::getenv("EDEN_NX2_CB24_TRACE") != nullptr; trace) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        static u32 logged{};
        if ((vs == 0x11471c9a00 || vs == 0x11471ca600 || vs == 0x11471cd300) && logged++ < 3) {
            std::array<u32, 24> words{};
            gpu_memory->ReadBlockUnsafe(regs.const_buffer.Address(), words.data(), sizeof(words));
            LOG_INFO(HW_GPU, "bank24 draw vs={:#x}: CB_ADDRESS {:#x} size {:#x} offset {:#x}: {:08x}", vs,
                     regs.const_buffer.Address(), regs.const_buffer.size, regs.const_buffer.offset,
                     fmt::join(words, " "));
            // Search every bound vertex-stage buffer for render-target-size values.
            const auto& stage = maxwell3d->state.shader_stages[0];
            for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& buffer = stage.const_buffers[slot];
                if (!buffer.enabled || buffer.size == 0) continue;
                std::vector<u32> data(std::min<u32>(buffer.size, 0x10000) / 4);
                gpu_memory->ReadBlockUnsafe(buffer.address, data.data(), data.size() * 4);
                for (size_t i = 0; i < data.size(); ++i) {
                    const u32 w = data[i];
                    if (w == 0x45200000 || w == 0x44b40000 || w == 0x39cccccd || w == 0x3a360b61 ||
                        w == 0x44a00000 || w == 0x44340000 || w == 2559 || w == 1439) {
                        LOG_INFO(HW_GPU, "  slot {} +{:#x} = {:#x}", slot, i * 4, w);
                    }
                }
            }
        }
    }
    // EDEN_NX2_TRACE_VS=<address>: describe the next few draws using that vertex shader like the
    // after-upscale trace does (render target, blend, fragment textures).
    static const u64 trace_vs = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_VS");
        return value ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    if (trace_vs != 0) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        static u32 traced{};
        if (vs == trace_vs && traced++ % 200 == 0 && traced < 1000) {
            Tegra::Engines::g_nx2_trace_draws_after_upscale = 1;
        }
    }
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        --Tegra::Engines::g_nx2_trace_draws_after_upscale;
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        LOG_INFO(HW_GPU, "after upscale: draw rt0 {:#x} {}x{} fmt {:#x} block {}/{}/{} pitch_linear {} depth {} array_pitch {:#x} ctrl {:#x} vs {:#x} ps {:#x}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.rt[0].tile_mode.block_width.Value(),
                 regs.rt[0].tile_mode.block_height.Value(), regs.rt[0].tile_mode.block_depth.Value(),
                 regs.rt[0].tile_mode.is_pitch_linear.Value(), regs.rt[0].depth.Value(),
                 regs.rt[0].array_pitch, regs.rt_control.count.Value(), vs, ps);
        // Fragment-stage texture handles (bound buffer 2) and the images they name.
        const auto& handles = maxwell3d->state.shader_stages[4].const_buffers[2];
        if (handles.enabled) {
            for (u32 i = 0; i < 24; ++i) {
                const u32 handle = gpu_memory->Read<u32>(handles.address + i * 4);
                if (handle == 0 || handle == 0xffffffff) continue;
                Tegra::Texture::TICEntry tic{};
                gpu_memory->ReadBlockUnsafe(regs.tex_header.Address() + u64{handle & 0xfffff} * sizeof(tic),
                                            &tic, sizeof(tic));
                LOG_INFO(HW_GPU, "  fs handle[{}] = {:#x}: {:#x} {}x{} format {:#x}", i, handle,
                         tic.Address(), tic.Width(), tic.Height(), static_cast<u32>(tic.format.Value()));
            }
        }
        const auto& bt = regs.blend_per_target[0];
        LOG_INFO(HW_GPU, "  blend enable {} per-target {} global op {} src {:#x} dst {:#x} | rt0 op {} src {:#x} dst {:#x} alpha {:#x}/{:#x} mask {:#x}",
                 regs.blend.enable[0], regs.blend_per_target_enabled,
                 static_cast<u32>(regs.blend.color_op), static_cast<u32>(regs.blend.color_source),
                 static_cast<u32>(regs.blend.color_dest), static_cast<u32>(bt.color_op),
                 static_cast<u32>(bt.color_source), static_cast<u32>(bt.color_dest),
                 static_cast<u32>(bt.alpha_source), static_cast<u32>(bt.alpha_dest),
                 regs.color_mask[0].raw);
    }
    // EDEN_NX2_SKIP_VS=<address>[,<address>...]: drop draws using those vertex shaders (debug aid).
    static const std::vector<u64> skip_list = [] {
        std::vector<u64> list;
        if (const char* value = std::getenv("EDEN_NX2_SKIP_VS")) {
            for (char* end = nullptr; *value; value = *end ? end + 1 : end) {
                list.push_back(std::strtoull(value, &end, 16));
                if (end == value) break;
            }
        }
        return list;
    }();
    if (!skip_list.empty()) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        if (std::find(skip_list.begin(), skip_list.end(), vs) != skip_list.end()) return;
    }
    if (VideoCommon::Nx2Diag::Active()) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        g_diag_draw = {is_indexed ? "indexed" : "draw",
                       is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
                       instance_count};
        if (VideoCommon::Nx2Diag::Ablates() && DiagAblated(*maxwell3d)) {
            DiagRecordDraw(*maxwell3d, *gpu_memory, "ablated", false);
            return;
        }
    }
    // EDEN_NX2_RT_TRACE: log the first draw to every distinct colour target (debug aid).
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        static std::set<std::tuple<GPUVAddr, u32, u32, u64, u64>> seen;
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        const auto key = std::make_tuple(rt.Address(), rt.width, static_cast<u32>(rt.format), vs, ps);
        if (rt.width >= 640 && seen.size() < 600 && seen.insert(key).second) {
            LOG_INFO(HW_GPU, "nx2 draw target {:#x} {}x{} format={:#x} vs={:#x} ps={:#x} blend={} (pairs so far {})",
                     rt.Address(), rt.width, rt.height, static_cast<u32>(rt.format), vs, ps,
                     regs.blend.enable[0], seen.size());
        }
    }
    PrepareDraw(is_indexed, [this, is_indexed, instance_count] {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const u32 num_instances{instance_count};
        const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};

        if (mesh_draw && mesh_emulation_vertices_per_group != 0) {
            const u32 vertices{mesh_emulation_groups * mesh_emulation_vertices_per_group};
            if (vertices == 0) {
                return;
            }
            if (mesh_emulation_indirect.buffer) {
                // With a task program the distribution pass wrote the vertex count.
                scheduler.Record([this, indirect = mesh_emulation_indirect](vk::CommandBuffer cmdbuf) {
                    if (scheduler.GraphicsPipelineMissing()) {
                        return;
                    }
                    cmdbuf.DrawIndirect(indirect.buffer,
                                        indirect.offset +
                                            Shader::Backend::SPIRV::MESH_EMULATION_DRAW_ARGS_OFFSET,
                                        1, 0);
                });
                return;
            }
            scheduler.Record([this, vertices](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                cmdbuf.Draw(vertices, 1, 0, 0);
            });
            return;
        }
        if (mesh_draw) {
            scheduler.Record([this, groups = draw_params.num_vertices](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                cmdbuf.DrawMeshTasksEXT(groups, 1, 1);
            });
            return;
        }
        scheduler.Record([this, draw_params](vk::CommandBuffer cmdbuf) {
            if (scheduler.GraphicsPipelineMissing()) {
                return;
            }
            if (draw_params.is_indexed) {
                cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                   draw_params.first_index, draw_params.base_vertex,
                                   draw_params.base_instance);
            } else {
                cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                            draw_params.base_vertex, draw_params.base_instance);
            }
        });

        // Log draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string params = is_indexed ?
                fmt::format("vertices={}, instances={}, firstIndex={}, baseVertex={}, baseInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.first_index, draw_params.base_vertex, draw_params.base_instance) :
                fmt::format("vertices={}, instances={}, firstVertex={}, firstInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.base_vertex, draw_params.base_instance);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                is_indexed ? "vkCmdDrawIndexed" : "vkCmdDraw", params, VK_SUCCESS);
        }
    });
}

void RasterizerVulkan::DrawIndirect() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    const auto& params = maxwell3d->draw_manager.indirect_state;
    if (VideoCommon::Nx2Diag::Active()) {
        g_diag_draw = {"indirect", static_cast<u32>(params.max_draw_counts), 0};
        if (VideoCommon::Nx2Diag::Ablates() && DiagAblated(*maxwell3d)) {
            DiagRecordDraw(*maxwell3d, *gpu_memory, "ablated", false);
            return;
        }
    }
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params] {
        if (mesh_draw) {
            // Indirect task/mesh draws would need vkCmdDrawMeshTasksIndirectEXT's layout.
            static bool logged{};
            if (!std::exchange(logged, true)) {
                LOG_WARNING(Render_Vulkan, "Indirect task/mesh draw skipped");
            }
            return;
        }
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([this, buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            scheduler.Record([this, draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([this, buffer_obj = buffer->Handle(), offset, params](vk::CommandBuffer cmdbuf) {
            if (scheduler.GraphicsPipelineMissing()) {
                return;
            }
            if (params.is_indexed) {
                cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                           static_cast<u32>(params.max_draw_counts),
                                           static_cast<u32>(params.stride));
            } else {
                cmdbuf.DrawIndirect(buffer_obj, offset, static_cast<u32>(params.max_draw_counts),
                                    static_cast<u32>(params.stride));
            }
        });

        // Log indirect draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string log_params = fmt::format("drawCount={}, stride={}",
                params.max_draw_counts, params.stride);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                params.is_indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
                log_params, VK_SUCCESS);
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerVulkan::DrawTexture() {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates(nullptr);

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager.draw_texture_state;
    const auto& sampler = texture_cache.GetSampler(draw_texture_state.src_sampler, false);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();
    if (frame_dump.Capturing()) {
        FrameDumpPass(framebuffer);
    }

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
}

void RasterizerVulkan::Clear(u32 layer_count) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Clear};
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        const auto& regs = maxwell3d->regs;
        LOG_INFO(HW_GPU, "after upscale: clear rt0 {:#x} {}x{} fmt {:#x} surface {:#x} colour {} {} {} {}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.clear_surface.raw, regs.clear_color[0],
                 regs.clear_color[1], regs.clear_color[2], regs.clear_color[3]);
    }
    TraceWindowTarget(*maxwell3d, *gpu_memory, "clear", layer_count);
    FlushWork();
    gpu_memory->FlushCaching();

    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }
    if (use_depth) {
        NoteCameraMotionClear();
    }

    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    if (frame_dump.Capturing()) {
        FrameDumpPass(framebuffer);
    }
    const VkExtent2D render_area = framebuffer->RenderArea();

    constexpr bool ENABLE_DEFERRED_CLEAR = true;
    const bool color_full_channels = regs.clear_surface.R && regs.clear_surface.G &&
                                     regs.clear_surface.B && regs.clear_surface.A;
    const bool stencil_partial = use_stencil && framebuffer->HasAspectStencilBit() &&
                                 regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0;
    const bool ds_used = use_depth || use_stencil;
    const bool ds_deferrable =
        !ds_used || ((!framebuffer->HasAspectDepthBit() || use_depth) &&
                     (!framebuffer->HasAspectStencilBit() || use_stencil) && !stencil_partial);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    VkRect2D default_scissor{};
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = (std::numeric_limits<s32>::max)();
    default_scissor.extent.height = (std::numeric_limits<s32>::max)();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = regs.clear_surface.layer,
        .layerCount = layer_count,
    };
    const auto clamp_rect_to_render_area = [render_area](VkRect2D& rect) -> bool {
        const auto clamp_axis = [](s32& offset, u32& extent, u32 limit) {
            auto clamp_offset = [&offset, limit]() {
                if (limit == 0) {
                    offset = 0;
                    return;
                }
                offset = std::clamp(offset, 0, static_cast<s32>(limit));
            };

            if (extent == 0) {
                clamp_offset();
                return;
            }
            if (offset < 0) {
                const u32 shrink = (std::min)(extent, static_cast<u32>(-offset));
                extent -= shrink;
                offset = 0;
            }
            if (limit == 0) {
                extent = 0;
                offset = 0;
                return;
            }
            if (offset >= s32(limit)) {
                offset = s32(limit);
                extent = 0;
                return;
            }
            const u64 end_coord = u64(offset) + extent;
            if (end_coord > limit) {
                extent = limit - u32(offset);
            }
        };

        clamp_axis(rect.offset.x, rect.extent.width, render_area.width);
        clamp_axis(rect.offset.y, rect.extent.height, render_area.height);
        return rect.extent.width != 0 && rect.extent.height != 0;
    };
    if (!clamp_rect_to_render_area(clear_rect.rect)) {
        return;
    }

    const bool clear_covers_render_area =
        clear_rect.rect.offset.x == 0 && clear_rect.rect.offset.y == 0 &&
        clear_rect.rect.extent.width >= render_area.width &&
        clear_rect.rect.extent.height >= render_area.height;
    const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && (!regs.clear_control.use_scissor || clear_covers_render_area) &&
                                 regs.clear_surface.layer == 0 &&
                                 !scheduler.IsRenderPassActive() &&
                                 (!use_color || color_full_channels) && ds_deferrable;
    if (!can_defer_clear) {
        scheduler.RequestRenderpass(framebuffer);
    }

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    UpdateViewportsState(regs);

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (!is_integer) {
            std::memcpy(clear_value.color.float32, regs.clear_color.data(), regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.uint32[i] = u32(f32(u64(int_size) << 1U) * regs.clear_color[i]);
        } else {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.int32[i] = s32(f32(s64(int_size - 1) << 1) * (regs.clear_color[i] - 0.5f));
        }

        if (color_full_channels) {
            if (can_defer_clear) {
                scheduler.DeferColorClear(framebuffer, color_attachment, clear_value);
            } else {
                scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                });
            }
        } else {
            u8 color_mask = u8(regs.clear_surface.R | regs.clear_surface.G << 1 | regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
            blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, use_depth, regs.clear_depth,
                                     u8(regs.stencil_front_mask), regs.clear_stencil,
                                     regs.stencil_front_func_mask, dst_region);
    } else if (can_defer_clear) {
        VkClearValue ds_value{};
        ds_value.depthStencil.depth = regs.clear_depth;
        ds_value.depthStencil.stencil = regs.clear_stencil;
        scheduler.DeferDepthStencilClear(framebuffer, ds_value);
    } else {
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

namespace {
/// A/B gate for the compute/WFI synchronization fixes. On by default, EGLANTINE_SYNC_FIX=0 restores
/// the previous behaviour.
bool SyncFixEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EGLANTINE_SYNC_FIX");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}
} // namespace

void RasterizerVulkan::DispatchCompute() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Compute};
    FlushWork();
    gpu_memory->FlushCaching();
    if (frame_dump.Capturing()) {
        frame_dump.EndPass();
        frame_dump.DrainIfFull();
    }

    u64 native_key{};
    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline(native_key)};
    if (!pipeline) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute pipeline unavailable");
        return;
    }
    if (kepler_compute->IsNativeMode() &&
        kepler_compute->ProcessOcarinaUpscale(Settings::GetCurrentProgramID(), native_key,
                                              pipeline->ShaderHash())) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    // Command list pages the dispatch writes are copied for the split's front right after it.
    buffer_cache.Nx2CollectListCopies(true);
    SCOPE_EXIT {
        buffer_cache.Nx2CollectListCopies(false);
    };
    if (!pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache,
                             texture_cache)) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute resources unavailable");
        return;
    }
    const auto& qmd{kepler_compute->launch_description};
    const auto publish_collision_map = [&] {
        if (native_key != 0x6e0ae200ee9aa220ULL ||
            (qmd.const_buffer_enable_mask & 1) == 0) {
            return;
        }
        const auto& constants = qmd.const_buffer_config[0];
        if (constants.size < 0x3c) {
            return;
        }
        // WaterSurfaceCollisionHeightMap exposes its output handle in CB0.
        const u32 handle = gpu_memory->Read<u32>(constants.Address() + 0x38);
        const u32 index = Tegra::Texture::TexturePair(handle, qmd.linked_tsc != 0).first;
        texture_cache.PublishComputeCollisionImage(index);
    };
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    if (frame_dump.Capturing()) {
        const u64 shader_hash = pipeline->ShaderHash();
        frame_dump.NoteKeys(std::array{shader_hash}, std::array{pipeline_cache.ComputeNativeKey()});
        if (frame_dump.CapturingSampled()) {
            boost::container::small_vector<FrameDump::Sampled, 16> sampled;
            for (const auto& image : pipeline->SampledImages()) {
                sampled.push_back({5, image.tic, &texture_cache.GetImageView(image.id),
                                   &texture_cache.GetSampler(image.sampler)});
            }
            frame_dump.NoteDispatchSampled(shader_hash, sampled);
        }
        if (frame_dump.CapturingBuffers()) {
            FrameDumpBuffers(true, std::array{shader_hash});
            std::optional<std::array<u32, 3>> indirect_grid;
            if (indirect_address) {
                indirect_grid = std::array{gpu_memory->Read<u32>(*indirect_address),
                                           gpu_memory->Read<u32>(*indirect_address + 4),
                                           gpu_memory->Read<u32>(*indirect_address + 8)};
            }
            frame_dump.NoteLaunch(shader_hash, {qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z},
                                  {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
                                  indirect_address, indirect_grid);
        }
    }
    SCOPE_EXIT {
        if (!frame_dump.Capturing()) {
            return;
        }
        boost::container::small_vector<ImageView*, 8> written;
        for (const VideoCommon::ImageViewId id : pipeline->WrittenImages()) {
            written.push_back(&texture_cache.GetImageView(id));
        }
        frame_dump.CaptureDispatch(pipeline->ShaderHash(), written);
    };

    // Earlier writes must be visible to the dispatch, its indirect arguments included, and what it
    // writes to the next render pass.
    const auto record_read_barrier = [this] {
        static constexpr VkMemoryBarrier READ_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        };
        const bool fix = SyncFixEnabled();
        scheduler.Record([fix](vk::CommandBuffer cmdbuf) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   fix ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   0, READ_BARRIER);
        });
        if (fix) {
            scheduler.NoteComputeWrites();
        }
    };
    if (indirect_address) {
        // DispatchIndirect
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        const auto post_op = VideoCommon::ObtainBufferOperation::DiscardWrite;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        scheduler.RequestOutsideRenderPassOperationContext();
        if (SyncFixEnabled()) {
            record_read_barrier();
        }
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),
                          indirect_offset = offset](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
                return;
            }
            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
        });
        SubmitNx2ListCopies();
        publish_collision_map();
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    if (dim[0] > max_dim[0] || dim[1] > max_dim[1] || dim[2] > max_dim[2]) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    record_read_barrier();
    scheduler.Record([pipeline, dim](vk::CommandBuffer cmdbuf) {
        if (!pipeline->IsBound()) {
            return;
        }
        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
    });
    SubmitNx2ListCopies();
    publish_collision_map();

    // Log compute dispatch
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = fmt::format("groupCountX={}, groupCountY={}, groupCountZ={}",
            dim[0], dim[1], dim[2]);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            "vkCmdDispatch", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
    case VideoCommon::QueryType::StreamingByteCount:
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        query_cache.CounterReset(type);
        return;
    default:
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::SubmitNx2ListCopies() {
    // Submitted at once: ReadGpuWrittenCommands waits on these copies, so they can't sit out
    // the longer DeferFlush interval (#161).
    if (buffer_cache.Nx2CopyListPages(scheduler.CurrentTick()) &&
        (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FenceFlush) || !scheduler.DeferFlush())) {
        VideoCommon::Nx2Profile::FlushOriginTag tag{VideoCommon::Nx2Profile::FlushOrigin::ListCopy};
        scheduler.Flush();
    }
}

void RasterizerVulkan::FlushSplitIdle() {
    if (buffer_cache.HasPendingNx2ListCopies()) {
        std::scoped_lock lock{buffer_cache.mutex};
        SubmitNx2ListCopies();
    }
    VideoCommon::Nx2Profile::FlushOriginTag tag{VideoCommon::Nx2Profile::FlushOrigin::IdleHook};
    scheduler.FlushDeferred();
}

VideoCore::RasterizerInterface::GpuWrittenList RasterizerVulkan::ReadGpuWrittenCommandList(
    DAddr addr, std::span<u8> out, bool past_sync) {
    if (!VideoCommon::GpuSplit::Get().Active() && buffer_cache.HasPendingNx2ListCopies()) {
        std::scoped_lock lock{buffer_cache.mutex};
        SubmitNx2ListCopies();
    }
    // No cache lock: the front only reads the copies, under their own lock.
    switch (buffer_cache.Nx2ReadListCopy(addr, out, past_sync)) {
    case BufferCache::Nx2ListRead::Copied:
        return GpuWrittenList::Copied;
    case BufferCache::Nx2ListRead::Synced:
        return GpuWrittenList::Synced;
    case BufferCache::Nx2ListRead::NeedsSync:
        return GpuWrittenList::NeedsSync;
    default:
        return GpuWrittenList::Unknown;
    }
}

void RasterizerVulkan::NoteGpuWrittenCommandListSynced(DAddr page) {
    buffer_cache.Nx2NoteListSynced(page);
}

void RasterizerVulkan::FlushQueryResults(DAddr addr, u64 size) {
    if (addr == 0 || size == 0 || !query_cache.SemiFlushRegion(addr, size)) {
        return;
    }
    WaitQueryResultsFence(addr);
}

bool RasterizerVulkan::WaitQueryResultsFence(DAddr addr) {
    // Below High GPU accuracy a fence runs its callback at once, so wait for an operation queued
    // behind the pending report writes instead. On the split's back the fence's submission may be
    // deferred, and the fencing thread would wait for this thread to submit it.
    // The state outlives this call: if the wait gives up, the operation may still run later.
    struct WaitState {
        std::mutex mutex;
        std::condition_variable cv;
        bool written{};
    };
    const auto state = std::make_shared<WaitState>();
    fence_manager.SyncOperation([state] {
        std::scoped_lock lock{state->mutex};
        state->written = true;
        state->cv.notify_all();
    });
    fence_manager.SignalFence([] {});
    scheduler.FlushDeferred();
    // Never hang the GPU thread on it: wait in slices, submitting deferred work again, and give up
    // after 2 s (the read may then see older report values).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::unique_lock lock{state->mutex};
    while (!state->cv.wait_for(lock, std::chrono::milliseconds(5),
                               [&state] { return state->written; })) {
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_WARNING(Render_Vulkan, "FlushQueryResults: gave up waiting for query results "
                                       "at {:#x} after 2 s",
                        addr);
            return false;
        }
        lock.unlock();
        scheduler.FlushDeferred();
        lock.lock();
    }
    return true;
}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> texture_area;
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_area = texture_cache.GetFlushArea(addr, size);
    }
    if (texture_area) {
        // The core disarmed the read's pages before asking: the buffer cache arms only pages it
        // doesn't count as armed, so it must forget these.
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.ForgetReadTraps(addr, size);
        return *texture_area;
    }
    if (buffer_cache.FlushesOnCpuRead()) {
        std::scoped_lock lock{buffer_cache.mutex};
        auto area = buffer_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

void RasterizerVulkan::PrepareRegionWrite(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0 || False(which & VideoCommon::CacheType::BufferCache)) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.ClearDownload(addr, size);
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    if (Settings::values.skip_cpu_inner_invalidation.GetValue()) {
        return;
    }
    // The GPU's own cached writes (constant buffer data, every draw): most land where no image,
    // query or shader was ever registered, which page_filter tells without the caches' locks.
    const auto any = [&](const VideoCommon::PageFilter& filter) {
        for (const auto& [addr, size] : sequences) {
            if (filter.Any(addr, size)) {
                return true;
            }
        }
        return false;
    };
    if (any(texture_cache.page_filter)) {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    if (any(query_cache.page_filter)) {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
        }
    }
    if (any(pipeline_cache.page_filter)) {
        pipeline_cache.InvalidateRegions(sequences);
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    DEBUG_ASSERT(addr != 0 || size != 0);
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            // The shader cache sees this write later (InvalidateGPUCache): proofs of programs
            // there must stop now, before the guest submits work that uses the new code.
            Tegra::Nx2NoteDeferredCodeWrite(addr, size);
            return true;
        }
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::OnCacheInvalidations(std::span<const std::pair<DAddr, u64>> ranges) {
    if (ranges.empty()) {
        return;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.ClaimCpuWrites(ranges);
        for (const auto& [addr, size] : ranges) {
            if (addr != 0 && size != 0) {
                buffer_cache.WriteMemory(addr, size);
            }
        }
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : ranges) {
            if (addr != 0 && size != 0) {
                texture_cache.WriteMemory(addr, size);
            }
        }
    }
    for (const auto& [addr, size] : ranges) {
        if (addr != 0 && size != 0) {
            pipeline_cache.InvalidateRegion(addr, size);
        }
    }
}

bool RasterizerVulkan::TryCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) return true;
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        // WriteMemory downloads GPU-written bytes elsewhere on the written pages, which only the
        // GPU thread may do: leave those pages to the fallback.
        const DAddr page_begin = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
        const DAddr page_end = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE);
        if (buffer_cache.IsRegionGpuModified(page_begin, page_end - page_begin)) return false;
        texture_cache.WriteMemory(addr, size);
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return true;
}

void RasterizerVulkan::InvalidateGPUCache() {
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size, false);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func, bool force_completion) {
    fence_manager.SignalFence(std::move(func), force_completion);
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    scheduler.RequestOutsideRenderPassOperationContext();
    if (SyncFixEnabled()) {
        scheduler.Record([flags](vk::CommandBuffer cmdbuf) {
            // Later work waits for the earlier work and sees its writes.
            static constexpr VkMemoryBarrier WFI_BARRIER{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(flags, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WFI_BARRIER);
        });
    } else {
        scheduler.Record([event = *wfi_event, flags](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetEvent(event, flags);
            cmdbuf.WaitEvents(event, flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, {}, {});
        });
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::SignalOrdering};
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::FragmentBarrier() {
    // The next draw breaks the render pass only if it reads what this one wrote (#161): Ocarina
    // ended ~5,900 render passes a second here.
    if (device.IsMoltenVK() || VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FragmentBarrier)) {
        scheduler.DeferFragmentBarrier();
        return;
    }
    // We already put barriers when a render pass finishes
    scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerVulkan::TiledCacheBarrier() {
    // TODO: Implementing tiled barriers requires rewriting a good chunk of the Vulkan backend
}

void RasterizerVulkan::FlushCommands() {
    if (draw_counter == 0) {
        return;
    }
    draw_counter = 0;
    // Fences with downloads or queries come here right after queueing: ~9,400 a second in
    // Hyrule Warriors: Age of Imprisonment fights. Batched like the fences themselves.
    if (!scheduler.DeferFlush()) {
        VideoCommon::Nx2Profile::FlushOriginTag tag{VideoCommon::Nx2Profile::FlushOrigin::Fence};
        scheduler.Flush();
    }
}

void RasterizerVulkan::TickFrame() {
    draw_counter = 0;
    // Debug server requests (docs/nx2/debug-mcp.md), taken between frames on the owning thread.
    if (VideoCore::Nx2Debug::HasRequests()) {
        if (auto capture = VideoCore::Nx2Debug::TakeCapture()) {
            frame_dump.Arm(std::move(capture->directory), capture->sampled, capture->buffers,
                           capture->id);
        }
        if (const auto change = VideoCore::Nx2Debug::TakeRejectChange()) {
            pipeline_cache.RefreshDebugRejections(change->unrejected);
            VideoCore::Nx2Debug::NoteRejectApplied(change->generation);
        }
        while (const auto read = VideoCore::Nx2Debug::TakeGpuRead()) {
            std::vector<Tegra::MemoryManager*> spaces;
            if (gpu_memory != nullptr) {
                spaces.push_back(gpu_memory);
            }
            {
                std::scoped_lock lock{config_mutex};
                for (const auto& [id, space] : address_spaces) {
                    if (space.gpu_memory != gpu_memory) {
                        spaces.push_back(space.gpu_memory);
                    }
                }
            }
            VideoCore::Nx2Debug::FinishGpuRead(
                read->id, ReadGpuMemory(spaces, read->gpu_address, read->size));
        }
    }
    frame_dump.TickFrame();
    if (VideoCommon::Nx2Diag::Active()) {
        auto& diag = VideoCommon::Nx2Diag::Diagnostics::Get();
        if (const u32 count = diag.QueryCount(); diag.Capturing() && count > 0 && g_diag_pool != nullptr) {
            // The capture ends here: wait for its draws and read what the probes counted.
            scheduler.Finish();
            std::vector<u64> samples(count);
            const VkResult result = device.GetLogical().GetQueryResults(
                **g_diag_pool, 0, count, sizeof(u64) * count, samples.data(), sizeof(u64),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result == VK_SUCCESS) {
                diag.SetSamples(std::move(samples));
            } else {
                LOG_ERROR(Render_Vulkan, "diag: reading the draw probe failed ({})", static_cast<int>(result));
            }
        }
        diag.TickFrame();
        if (DiagLite() && VideoCommon::Nx2Perf::Verify()) {
            DiagLiteVerifyFrame();
        }
        if (diag.Capturing() && VideoCommon::Nx2Diag::SamplesEnabled()) {
            // A capture just began: (re)create and reset the probe pool.
            if (g_diag_pool == nullptr) {
                g_diag_pool = new vk::QueryPool(device.GetLogical().CreateQueryPool({
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_OCCLUSION,
                    .queryCount = VideoCommon::Nx2Diag::MaxProbeQueries,
                    .pipelineStatistics = 0,
                }));
            }
            if (device.IsHostQueryResetSupported()) {
                device.GetLogical().ResetQueryPool(**g_diag_pool, 0, VideoCommon::Nx2Diag::MaxProbeQueries);
            } else {
                scheduler.RequestOutsideRenderPassOperationContext();
                scheduler.Record([pool = **g_diag_pool](vk::CommandBuffer cmdbuf) {
                    cmdbuf.ResetQueryPool(pool, 0, VideoCommon::Nx2Diag::MaxProbeQueries);
                });
            }
        }
    }
    pipeline_cache.TickFrame();
    guest_descriptor_queue.TickFrame();
    compute_pass_descriptor_queue.TickFrame();
    descriptor_buffer_ring.TickFrame();
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

bool RasterizerVulkan::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}

bool RasterizerVulkan::HasDrawTransformFeedback() {
    return device.IsTransformFeedbackDrawSupported();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

bool RasterizerVulkan::UsesNvidiaDlss() const {
    return texture_cache_runtime.UsesNvidiaDlss();
}

bool RasterizerVulkan::SupportsGameDlssNetwork() const {
    // Compute pipelines are created 32 threads wide where the device can be told to.
    return device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT);
}

std::optional<VideoCore::GameDlssProbe> RasterizerVulkan::ProbeGameDlssOutput(
    const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.ProbeGameDlssOutput(dst, src);
}

bool RasterizerVulkan::AccelerateTemporalUpscale(
    const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
    const Tegra::Engines::Fermi2D::Surface& motion_vectors, const Tegra::Texture::TICEntry* depth,
    std::array<f32, 2> jitter, std::array<f32, 2> motion_scale) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.TemporalUpscale(dst, src, motion_vectors, depth, jitter, motion_scale);
}

// NVIDIA DLSS (DLAA and Super Resolution) in place of Ocarina of Time's post-tonemap anti-aliasing
// and upscaling (ocarina_upscale.h). The game draws no jitter and no motion vectors, so the host
// offsets the viewport of every draw of the scene's camera at the scene's size by a sub-pixel
// jitter (the depth prepass and the colour pass alike, or the colour pass's equal depth test
// fails), and DLSS gets motion vectors from the camera alone: each pixel's depth taken back
// through this frame's camera and forward through the last one's.
void RasterizerVulkan::NoteCameraMotionDraw() {
    namespace Ocarina = VideoCommon::OcarinaUpscale;
    auto& motion = camera_motion;
    if (!motion.armed) {
        return;
    }
    if (!UsesNvidiaDlss()) {
        motion.armed = false;
        motion.jitter_active = false;
        motion.jitter_ready = false;
        if (motion.applied != std::array<f32, 2>{}) {
            motion.applied = {};
            state_tracker.InvalidateViewports();
        }
        return;
    }
    const auto& regs = maxwell3d->regs;
    // Exclude occlusion-query proxy draws (sample-count query enabled with no depth, stencil, or
    // color writes) and point topology draws (D3): Ocarina of Time's fog and sun bounding-box /
    // sphere visibility queries must not be jittered.
    const bool sample_query_only =
        regs.zpass_pixel_count_enable != 0 && !regs.depth_write_enabled && !regs.stencil_enable &&
        (regs.rt_control.count == 0 || regs.rt[0].format == Tegra::RenderTargetFormat::NONE ||
         (regs.color_mask[0].R == 0 && regs.color_mask[0].G == 0 && regs.color_mask[0].B == 0 &&
          regs.color_mask[0].A == 0));
    const bool excluded_draw =
        sample_query_only ||
        maxwell3d->draw_manager.draw_state.topology == Maxwell::PrimitiveTopology::Points;
    const GPUVAddr depth = !excluded_draw && regs.zeta_enable &&
                                   regs.viewport_scale_offset_enabled &&
                                   regs.zeta.format == Tegra::DepthFormat::Z32_FLOAT
                               ? regs.zeta.Address()
                               : 0;
    const std::array<u32, 2> depth_size{regs.zeta_size.width, regs.zeta_size.height};
    // The scene's geometry binds its camera. Full-screen passes on the same depth buffer (the
    // tonemap among them) don't, and stay where they are.
    const auto& camera = maxwell3d->state.shader_stages[0].const_buffers[Ocarina::CameraBuffer];
    const bool has_camera = depth != 0 && camera.enabled && camera.size >= Ocarina::CameraBytes;
    auto& cached = motion.camera;
    if (has_camera && camera.address != cached.address) {
        cached.address = camera.address;
        cached.perspective = false;
        if (gpu_memory->IsFullyMappedRange(camera.address, Ocarina::CameraBytes)) {
            std::array<f32, 16> projection;
            gpu_memory->ReadBlockUnsafe(camera.address + Ocarina::ProjectionOffset,
                                        projection.data(), sizeof(projection));
            cached.perspective = Ocarina::IsPerspective(projection);
            gpu_memory->ReadBlockUnsafe(camera.address + Ocarina::ViewProjectionOffset,
                                        cached.view_projection.data(),
                                        sizeof(cached.view_projection));
        }
    }
    const bool perspective = has_camera && cached.perspective;
    // Every depth buffer the scene's camera draws at the scene's size, not only the scene's: the
    // game shades its opaque geometry with an EQUAL test against a copy of the prepass depth, so
    // both must have the same offset.
    std::array<f32, 2> jitter{};
    if (perspective && motion.jitter_active && depth_size == motion.jitter_depth_size) {
        jitter = motion.jitter;
    }
    if (perspective) {
        CameraMotionScene* scene = nullptr;
        for (size_t i = 0; i < motion.num_scenes; ++i) {
            CameraMotionScene& candidate = motion.scenes[i];
            if (candidate.depth == depth && candidate.depth_info.size.width == depth_size[0] &&
                candidate.depth_info.size.height == depth_size[1]) {
                scene = &candidate;
            }
        }
        if (!scene) {
            // Shadow and effect passes draw smaller depth buffers before the scene's: when the
            // records are full, a larger buffer takes the smallest one's.
            const auto area = [](const CameraMotionScene& record) {
                return u64{record.depth_info.size.width} * record.depth_info.size.height;
            };
            if (motion.num_scenes < motion.scenes.size()) {
                scene = &motion.scenes[motion.num_scenes++];
            } else if (CameraMotionScene& smallest =
                           *std::ranges::min_element(motion.scenes, {}, area);
                       area(smallest) < u64{depth_size[0]} * depth_size[1]) {
                scene = &smallest;
            }
            if (scene) {
                *scene = CameraMotionScene{};
                scene->depth = depth;
                scene->depth_info = VideoCommon::ImageInfo(regs.zeta, regs.zeta_size,
                                                           regs.anti_alias_samples_mode);
            }
        }
        if (scene) {
            const bool rescaled = texture_cache.IsRescaling();
            const float scale = rescaled ? Settings::values.resolution_info.up_factor : 1.0f;
            scene->camera = camera.address;
            scene->view_projection = cached.view_projection;
            scene->viewport = GetViewportState(device, regs, 0, scale);
            scene->y_sign = device.IsNvViewportSwizzleSupported() &&
                                    regs.viewport_transform[0].swizzle.y ==
                                        Maxwell::ViewportSwizzle::NegativeY
                                ? -1.0f
                                : 1.0f;
            scene->depth_minus_one = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
            scene->rescaled = rescaled;
            scene->jitter = jitter;
            ++scene->draws;
        }
    }
    if (jitter != motion.applied) {
        motion.applied = jitter;
        state_tracker.InvalidateViewports();
    }
}

void RasterizerVulkan::NoteCameraMotionClear() {
    auto& motion = camera_motion;
    const auto& regs = maxwell3d->regs;
    if (!motion.armed || !regs.zeta_enable || regs.zeta.Address() != motion.jitter_depth ||
        std::array<u32, 2>{regs.zeta_size.width, regs.zeta_size.height} !=
            motion.jitter_depth_size) {
        return;
    }
    if (motion.jitter_ready) {
        motion.jitter_ready = false;
        motion.jitter_active = true;
        return;
    }
    // Cleared again with no evaluation since: another clear in the same frame, or the game's
    // anti-aliasing stopped running (a menu, a load), so its draws mustn't keep the jitter.
    if (motion.jitter_active && motion.last_evaluation &&
        std::chrono::steady_clock::now() - *motion.last_evaluation >
            std::chrono::milliseconds{100}) {
        motion.jitter_active = false;
    }
}

bool RasterizerVulkan::AccelerateCameraMotionUpscale(const Tegra::Engines::Fermi2D::Surface& src,
                                                     const Tegra::Engines::Fermi2D::Surface& dst) {
    namespace Ocarina = VideoCommon::OcarinaUpscale;
    auto& motion = camera_motion;
    motion.armed = true;
    // The scene: the depth buffer at the colour's size with the most perspective draws.
    const CameraMotionScene* scene = nullptr;
    for (size_t i = 0; i < motion.num_scenes; ++i) {
        const CameraMotionScene& candidate = motion.scenes[i];
        if (candidate.draws > 0 && candidate.depth_info.size.width == src.width &&
            candidate.depth_info.size.height == src.height &&
            (!scene || candidate.draws > scene->draws)) {
            scene = &candidate;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const auto finish = [&](bool done) {
        motion.num_scenes = 0;
        // Camera buffers are rewritten each frame, often at the same addresses.
        motion.camera = {};
        if (!done) {
            // The game's own anti-aliasing draws this frame: no jitter, and no history to keep.
            motion.jitter_active = false;
            motion.jitter_ready = false;
            motion.jitter_depth = 0;
            motion.jitter_depth_size = {};
            motion.previous_screen.reset();
        }
        return done;
    };
    if (!scene) {
        static u32 no_scene_logged{};
        if (no_scene_logged < 4) {
            ++no_scene_logged;
            LOG_INFO(Render_Vulkan,
                     "Ocarina of Time anti-aliasing: no matching scene for {}x{} -> {}x{} "
                     "({} recorded scenes)",
                     src.width, src.height, dst.width, dst.height, motion.num_scenes);
            for (size_t i = 0; i < motion.num_scenes; ++i) {
                const auto& candidate = motion.scenes[i];
                LOG_INFO(Render_Vulkan, "  scene[{}]: depth={:#x} {}x{} draws={} camera={:#x}", i,
                         candidate.depth, candidate.depth_info.size.width,
                         candidate.depth_info.size.height, candidate.draws, candidate.camera);
            }
        }
        return finish(false);
    }
    // The scene's viewport must cover the image, so the matrix maps to its pixels.
    const VkViewport& viewport = scene->viewport;
    const f32 width = static_cast<f32>(
        scene->rescaled ? Settings::values.resolution_info.ScaleUp(src.width) : src.width);
    const f32 height = static_cast<f32>(
        scene->rescaled ? Settings::values.resolution_info.ScaleUp(src.height) : src.height);
    if (std::abs(viewport.x) > 0.5f || std::abs(viewport.width - width) > 0.5f ||
        std::abs(std::min(viewport.y, viewport.y + viewport.height)) > 0.5f ||
        std::abs(std::abs(viewport.height) - height) > 0.5f) {
        if (motion.evaluations < 4) {
            LOG_INFO(Render_Vulkan,
                     "Ocarina of Time anti-aliasing: viewport ({}, {}, {}x{}) does not cover "
                     "{}x{}",
                     viewport.x, viewport.y, viewport.width, viewport.height, width, height);
        }
        return finish(false);
    }
    // Device depth = offset + scale * NDC z, as the viewport's depth range gives it.
    const f32 depth_offset = scene->depth_minus_one
                                 ? (viewport.minDepth + viewport.maxDepth) * 0.5f
                                 : viewport.minDepth;
    const f32 depth_scale = scene->depth_minus_one
                                ? (viewport.maxDepth - viewport.minDepth) * 0.5f
                                : viewport.maxDepth - viewport.minDepth;
    const Ocarina::Matrix screen = Ocarina::ScreenMatrix(
        Ocarina::ViewportMatrix(viewport.x, viewport.y, viewport.width, viewport.height,
                                scene->y_sign, depth_offset, depth_scale),
        scene->view_projection);
    const std::array<u32, 2> size{src.width, src.height};
    // A gap in the evaluations (a menu, a load) or another scene starts the history again.
    bool reset = !motion.previous_screen || motion.previous_depth != scene->depth ||
                 motion.previous_size != size || motion.previous_rescaled != scene->rescaled ||
                 !motion.last_evaluation ||
                 now - *motion.last_evaluation > std::chrono::milliseconds{250};
    std::optional<std::array<f32, 16>> reprojection;
    if (!reset) {
        reprojection = Ocarina::Reprojection(*motion.previous_screen, screen);
    }
    if (!reprojection) {
        reset = true;
        reprojection = std::array<f32, 16>{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    }
    const std::array<f32, 2> jitter = scene->jitter;
    bool done;
    {
        std::scoped_lock lock{texture_cache.mutex};
        done = texture_cache.CameraMotionUpscale(
            dst, src, scene->depth_info, scene->depth,
            {static_cast<u32>(width), static_cast<u32>(height)}, *reprojection, jitter, reset);
    }
    if (motion.evaluations < 4 || (!done && motion.evaluations < 64)) {
        LOG_INFO(Render_Vulkan,
                 "Ocarina of Time anti-aliasing: NVIDIA DLSS {} (scene depth {:#x} {}x{} -> "
                 "{}x{}, {} draws, camera {:#x}, jitter {},{}, reset {})",
                 done ? "applied" : "unavailable; the game's own runs", scene->depth, src.width,
                 src.height, dst.width, dst.height, scene->draws, scene->camera, jitter[0],
                 jitter[1], reset);
    }
    ++motion.evaluations;
    if (!done) {
        return finish(false);
    }
    motion.jittered |= scene->jitter != std::array<f32, 2>{};
    if (motion.evaluations == 120 && !motion.jittered) {
        LOG_WARNING(Render_Vulkan,
                    "Ocarina of Time anti-aliasing: the scene's depth buffer at {:#x} is never "
                    "cleared, so its draws get no jitter",
                    scene->depth);
    }
    motion.previous_screen = screen;
    motion.previous_depth = scene->depth;
    motion.previous_size = size;
    motion.previous_rescaled = scene->rescaled;
    motion.last_evaluation = now;
    // The next frame's jitter, from when its scene depth is cleared; the rest of this frame (its
    // interface) is drawn without it.
    motion.jitter_depth = scene->depth;
    motion.jitter_depth_size = size;
    motion.jitter = Ocarina::Jitter(++motion.phase);
    motion.jitter_ready = true;
    motion.jitter_active = false;
    return finish(true);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (gpu_memory->UsesNativeTiling()) {
        u32 first_word{};
        if (memory.size() >= sizeof(first_word)) std::memcpy(&first_word, memory.data(), sizeof(first_word));
        VideoCommon::TraceNativeQuery(11, gpu_memory->GetID(), address, cpu_addr.value_or(0),
            0, first_word, static_cast<u32>(copy_size));
        static const GPUVAddr watch = [] {
            const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
            return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
        }();
        if (watch != 0 && address < watch + 16 && address + copy_size > watch) {
            LOG_INFO(Render_Vulkan, "Semaphore watch: inline write {:#x} size {} first word {:#x}",
                     address, copy_size, first_word);
        }
    }
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    // EDEN_NX2_INLINE_DEFER=0: invalidate every cache at once (for comparisons).
    static const bool defer = [] {
        const char* value = std::getenv("EDEN_NX2_INLINE_DEFER");
        return value == nullptr || value[0] != '0';
    }();
    // One cache at a time: holding both while waiting for either (std::lock) waited longer, the
    // fencing thread and CPU write traps holding one of them most of the time.
    bool uploaded;
    {
        std::scoped_lock lock{buffer_cache.mutex};
        // Guest memory has the words now: a command list copy of the page no longer does.
        buffer_cache.Nx2NoteListHostWrite(*cpu_addr, copy_size, memory);
        uploaded = buffer_cache.InlineMemory(*cpu_addr, copy_size, memory);
        if (!uploaded && !defer) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    // Pending query results must not land over the new words later: drop them now.
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
    if (!uploaded && defer) {
        // Title 1 makes thousands of small inline writes between draws (~10% of the GPU
        // thread invalidating every cache for each). Guest memory is written; the buffer,
        // texture and shader caches see the merged ranges at the next FlushCaching, which runs
        // before anything of theirs is used. Writes into GPU-written buffer data were uploaded
        // in place above, as before.
        gpu_memory->AccumulateInvalidation(address, copy_size);
        return;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return {};
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    static u32 display_trace_count{};
    static const bool frame_capture = std::getenv("EDEN_NATIVE_FRAME_CAPTURE") != nullptr;
    if (frame_capture && display_trace_count++ < 64) {
        LOG_INFO(HW_GPU, "Native display source: address={:#x}, offset={:#x}, dimensions={}x{}, stride={}, accelerated={}, mask={:#x}",
                 framebuffer_addr, config.offset, config.width, config.height, pixel_stride,
                 image_view != nullptr, config.layer_stack_mask);
    }
    if (!image_view) {
        return {};
    }
    if (frame_dump.Capturing()) {
        frame_dump.EndPass();
        frame_dump.CapturePresent(*image_view);
    }

    query_cache.NotifySegment(false);

    const auto& resolution = Settings::values.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

bool RasterizerVulkan::FrameDumpBuffers(bool compute, std::span<const u64> shader_hashes) {
    static constexpr u32 BUFFER_CAP = 4U << 20;
    std::vector<FrameDump::BoundBuffer> buffers;
    buffer_cache.ForEachBoundBuffer(compute, [&](u32 stage, bool storage, u32 index, bool written,
                                                 const auto& binding, u32 bound_size,
                                                 const auto* host, u32 host_offset) {
        const u32 size = (std::min)(bound_size, BUFFER_CAP);
        const bool gpu_modified = buffer_cache.IsRegionGpuModified(binding.device_addr, size);
        FrameDump::BoundBuffer& buffer = buffers.emplace_back(FrameDump::BoundBuffer{
            .stage = stage,
            .storage = storage,
            .index = index,
            .written = written,
            .gpu_addr = binding.gpu_addr,
            .device_addr = binding.device_addr,
            .bound_size = bound_size,
            .size = size,
            .gpu_modified = gpu_modified,
            .host = host ? host->Handle() : VK_NULL_HANDLE,
            .host_offset = host_offset,
            .guest = {},
        });
        if (!gpu_modified || !host) {
            buffer.guest.resize(size);
            device_memory.ReadBlockUnsafe(binding.device_addr, buffer.guest.data(), size);
        }
    });
    return frame_dump.NoteBuffers(compute, shader_hashes, buffers);
}

void RasterizerVulkan::FrameDumpPass(const Framebuffer* framebuffer) {
    std::array<ImageView*, VideoCommon::NUM_RT> colors{};
    const auto& ids = texture_cache.GetRenderTargets().color_buffer_ids;
    for (size_t slot = 0; slot < ids.size(); ++slot) {
        if (ids[slot]) {
            colors[slot] = &texture_cache.GetImageView(ids[slot]);
        }
    }
    if (frame_dump.BeginPass(framebuffer, colors)) {
        scheduler.RequestRenderpass(framebuffer);
    }
}

void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr u32 DRAWS_TO_DISPATCH = 512;
    static constexpr u32 CHECK_MASK = 3;
#else
    // Every 32 draws (was 8): each dispatch woke the worker thread, and Switch 2 titles make
    // ~55,000 draws a second. A full chunk is dispatched on its own (Scheduler::Record).
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
    static constexpr u32 CHECK_MASK = 31;
#endif // __ANDROID__
    static_assert(DRAWS_TO_DISPATCH % (CHECK_MASK + 1) == 0);
    if (submit_while_waited) {
        scheduler.NoteDraw();
    }
    if ((++draw_counter & CHECK_MASK) != 0) {
        return;
    }
    if (submit_while_waited && Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
        VideoCommon::GpuSplit::OnBackThread() && SubmitWhileWaited()) {
        draw_counter = 0;
        return;
    }
    // Keep the host GPU working while native commands are still being recorded.
    // The 128-draw native budget is tuned for MoltenVK; desktop Vulkan takes the legacy 4096 (#161).
    static const u32 native_flush_draws = [] {
        const u32 fallback = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::SubmitBudget) ? 4096U : 128U;
        const char* value = std::getenv("EDEN_NX2_FLUSH_DRAWS");
        if (value == nullptr || value[0] < '0' || value[0] > '9') {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (*end != '\0' || parsed == 0) {
            return fallback;
        }
        return static_cast<u32>(std::clamp(parsed, 32UL, 1'048'576UL));
    }();
    const u32 draws_to_dispatch = Tegra::g_nx2_ampere.load(std::memory_order_relaxed)
                                     ? native_flush_draws
                                     : DRAWS_TO_DISPATCH;
    if (draw_counter < draws_to_dispatch) {
        scheduler.DispatchWork();
        return;
    }
    {
        VideoCommon::Nx2Profile::FlushOriginTag tag{VideoCommon::Nx2Profile::FlushOrigin::Budget};
        scheduler.Flush();
    }
    draw_counter = 0;
}

bool RasterizerVulkan::SubmitWhileWaited() {
    // A command list copy or a fence submitted later waits behind everything recorded since the
    // last submission: the host GPU starts on it now while the front can't use the time, or the
    // GPU would sit idle. The same commands in the same order; only the submissions move, at a
    // draw boundary on the back, as fences and list copies already submit.
    namespace Profile = VideoCommon::Nx2Profile;
    const u32 draws = scheduler.DrawsSinceSubmit();
    Profile::FlushOrigin origin;
    if (draws >= submit_waited_draws && VideoCommon::GpuSplit::Get().FrontWaitsForBack()) {
        origin = Profile::FlushOrigin::Waited;
    } else if (draws >= submit_idle_draws && scheduler.GpuCaughtUp()) {
        origin = Profile::FlushOrigin::GpuIdle;
    } else {
        return false;
    }
    Profile::FlushOriginTag tag{origin};
    if (!VideoCommon::Nx2Perf::Verify()) {
        scheduler.Flush();
        return true;
    }
    // No answer to compare: the submission must end the pass and advance one tick, as any.
    const u64 tick = scheduler.CurrentTick();
    scheduler.Flush();
    if (scheduler.CurrentTick() != tick + 1 || scheduler.IsRenderPassActive() ||
        scheduler.DrawsSinceSubmit() != 0) {
        static std::atomic<u64> mismatches{};
        if (const u64 count = mismatches.fetch_add(1, std::memory_order_relaxed) + 1; count <= 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf submit_while_waited: tick {} -> {}, render pass {}, {} draws left "
                      "after the submit ({} so far)",
                      tick, scheduler.CurrentTick(), scheduler.IsRenderPassActive(),
                      scheduler.DrawsSinceSubmit(), count);
        }
    }
    return true;
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.Nx2CollectListCopies(true);
    const bool copied = buffer_cache.DMACopy(src_address, dest_address, amount);
    if (buffer_cache.Nx2CopyListPages(scheduler.CurrentTick())) {
        VideoCommon::Nx2Profile::FlushOriginTag tag{VideoCommon::Nx2Profile::FlushOrigin::ListCopy};
        scheduler.Flush();
    }
    buffer_cache.Nx2CollectListCopies(false);
    return copied;
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

void RasterizerVulkan::UpdateDynamicStates(const GraphicsPipeline* pipeline) {
    auto& regs = maxwell3d->regs;
    auto& flags = maxwell3d->dirty.flags;
    // A mesh emulation pipeline has static input assembly and vertex input. Binding it replaces
    // that dynamic state, so leave it to the next regular draw to set again.
    const bool emulated_mesh{pipeline != nullptr && pipeline->IsEmulatedMesh()};
    if (emulated_mesh) {
        state_tracker.InvalidateInputState();
    }
    const auto topology = maxwell3d->draw_manager.draw_state.topology;
    const bool topology_changed = !emulated_mesh && state_tracker.ChangePrimitiveTopology(topology);
    if (topology_changed) {
        flags[Dirty::DepthBiasEnable] = true;
        flags[Dirty::PrimitiveRestartEnable] = true;
    }

    static_assert(sizeof(Tegra::Engines::Maxwell3D::DirtyState::Flags) == 32);
    static constexpr auto MakeDirtyWords = [](std::initializer_list<u8> indices) {
        std::array<u64, 4> words{};
        for (const u8 idx : indices) {
            words[idx / 64] |= 1ULL << (idx % 64);
        }
        return words;
    };
    static constexpr auto HasAnyDirty = [](const std::array<u64, 4>& words,
                                           const std::array<u64, 4>& mask) noexcept {
        return ((words[0] & mask[0]) | (words[1] & mask[1]) | (words[2] & mask[2]) |
                (words[3] & mask[3])) != 0;
    };
    static constexpr auto kViewportScissorMask = MakeDirtyWords({
        Dirty::Viewports,
        VideoCommon::Dirty::RescaleViewports,
        Dirty::Scissors,
        VideoCommon::Dirty::RescaleScissors,
    });
    static constexpr auto kBaseDynamicMask = MakeDirtyWords({
        Dirty::DepthBias,
        VideoCommon::Dirty::DepthBiasGlobal,
        Dirty::BlendConstants,
        Dirty::DepthBounds,
        Dirty::StencilProperties,
        Dirty::LineWidth,
    });
    static constexpr auto kEds1Mask = MakeDirtyWords({
        Dirty::CullMode,
        Dirty::DepthCompareOp,
        Dirty::FrontFace,
        Dirty::StencilOp,
        Dirty::StateEnable,
    });
    static constexpr auto kEds2Mask = MakeDirtyWords({
        Dirty::PrimitiveRestartEnable,
        Dirty::RasterizerDiscardEnable,
        Dirty::DepthBiasEnable,
    });
    static constexpr auto kEds3EnablesMask = MakeDirtyWords({
        Dirty::LogicOpEnable,
        Dirty::DepthClampEnable,
        Dirty::LineRasterizationMode,
        Dirty::LineStippleEnable,
        Dirty::ConservativeRasterizationMode,
        Dirty::AlphaToCoverageEnable,
        Dirty::AlphaToOneEnable,
    });
    static constexpr auto kEds3BlendMask = MakeDirtyWords({
        Dirty::Blending,
        Dirty::ColorMask,
    });
    static constexpr auto kAllDynamicMask = [] {
        std::array<u64, 4> combined{};
        for (const auto& mask : {kViewportScissorMask, kBaseDynamicMask, kEds1Mask, kEds2Mask,
                                 kEds3EnablesMask, kEds3BlendMask}) {
            for (size_t i = 0; i < 4; ++i) {
                combined[i] |= mask[i];
            }
        }
        combined[Dirty::LogicOp / 64] |= 1ULL << (Dirty::LogicOp % 64);
        return combined;
    }();

    const u32 color_output_mask = pipeline ? pipeline->ColorOutputMask() : ~0U;
    const bool color_output_changed = color_output_mask != last_color_output_mask;
    const auto words = std::bit_cast<std::array<u64, 4>>(flags);
    if (topology_changed || color_output_changed || HasAnyDirty(words, kAllDynamicMask)) {
        if (HasAnyDirty(words, kViewportScissorMask)) {
            UpdateViewportsState(regs);
            UpdateScissorsState(regs);
        }
        if (HasAnyDirty(words, kBaseDynamicMask)) {
            UpdateDepthBias(regs);
            UpdateBlendConstants(regs);
            UpdateDepthBounds(regs);
            UpdateStencilFaces(regs);
            UpdateLineWidth(regs);
        }

        if ((topology_changed || HasAnyDirty(words, kEds1Mask)) &&
            device.IsExtExtendedDynamicStateSupported()) {
            UpdateCullMode(regs);
            UpdateDepthCompareOp(regs);
            UpdateFrontFace(regs);
            UpdateStencilOp(regs);
            if (state_tracker.TouchStateEnable()) {
                UpdateDepthBoundsTestEnable(regs);
                UpdateDepthTestEnable(regs);
                UpdateDepthWriteEnable(regs);
                UpdateStencilTestEnable(regs);
            }
            if (topology_changed) {
                scheduler.Record([topology_vk = MaxwellToVK::PrimitiveTopology(device, topology)](
                                     vk::CommandBuffer cmdbuf) {
                    cmdbuf.SetPrimitiveTopologyEXT(topology_vk);
                });
            }
        }

        if (HasAnyDirty(words, kEds2Mask) && device.IsExtExtendedDynamicState2Supported()) {
            if (!emulated_mesh) {
                UpdatePrimitiveRestartEnable(regs);
            }
            UpdateRasterizerDiscardEnable(regs);
            UpdateDepthBiasEnable(regs);
        }

        if (flags[Dirty::LogicOp] && device.IsExtExtendedDynamicState2ExtrasSupported()) {
            UpdateLogicOp(regs);
        }

        if (HasAnyDirty(words, kEds3EnablesMask) &&
            device.IsExtExtendedDynamicState3EnablesSupported()) {
            using namespace Tegra::Engines;
            // AMD Workaround: LogicOp incompatible with float render targets
            if (flags[Dirty::LogicOpEnable] && regs.logic_op.enable &&
                (device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                 device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_PROPRIETARY)) {
                const auto has_float = std::any_of(
                    regs.vertex_attrib_format.begin(), regs.vertex_attrib_format.end(),
                    [](const auto& attrib) {
                        return attrib.type == Maxwell3D::Regs::VertexAttribute::Type::Float;
                    });
                regs.logic_op.enable = static_cast<u32>(!has_float);
            }
            UpdateLogicOpEnable(regs);
            UpdateDepthClampEnable(regs);
            UpdateLineRasterizationMode(regs);
            UpdateLineStippleEnable(regs);
            UpdateConservativeRasterizationMode(regs);
            UpdateAlphaToCoverageEnable(regs);
            UpdateAlphaToOneEnable(regs);
        }

        if (HasAnyDirty(words, kEds3BlendMask) || color_output_changed) {
            if (device.IsExtExtendedDynamicState3BlendingSupported()) {
                UpdateBlending(regs, pipeline);
            } else if (device.IsExtColorWriteEnableSupported()) {
                UpdateColorWriteEnable(regs);
            }
        }
    }

    if (pipeline && pipeline->HasDynamicVertexInput() && !emulated_mesh &&
        device.IsExtVertexInputDynamicStateSupported() &&
        (flags[Dirty::VertexInput] || flags[VideoCommon::Dirty::VertexBuffers])) {
        UpdateVertexInput(regs);
    }
}

void RasterizerVulkan::HandleTransformFeedback() {
    static std::once_flag warn_unsupported;

    const auto& regs = maxwell3d->regs;
    if (!device.IsExtTransformFeedbackSupported()) {
        if (regs.transform_feedback_enabled != 0) {
            std::call_once(warn_unsupported, [&] {
                LOG_WARNING(Render_Vulkan, "Transform feedback requested by guest but VK_EXT_transform_feedback is unavailable; queries disabled");
            });
        } else {
            std::call_once(warn_unsupported, [&] {
                LOG_INFO(Render_Vulkan, "VK_EXT_transform_feedback not available on device");
            });
        }
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        // Log extension usage for transform feedback
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_transform_feedback", "HandleTransformFeedback");
        }
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchViewports()) {
        return;
    }

    maxwell3d->dirty.flags[Dirty::Scissors] = true;

    if (!regs.viewport_scale_offset_enabled) {
        float x = static_cast<float>(regs.surface_clip.x);
        float y = static_cast<float>(regs.surface_clip.y);
        float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
        float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y += height;
            height = -height;
        }
        VkViewport viewport{
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        scheduler.Record([this, viewport](vk::CommandBuffer cmdbuf) {
            const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkViewport, Maxwell::NumViewports> viewport_list{};
            viewport_list.fill(viewport);
            const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
            cmdbuf.SetViewport(0, viewports);
        });
        return;
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    std::array viewport_list{
        GetViewportState(device, regs, 0, scale),  GetViewportState(device, regs, 1, scale),
        GetViewportState(device, regs, 2, scale),  GetViewportState(device, regs, 3, scale),
        GetViewportState(device, regs, 4, scale),  GetViewportState(device, regs, 5, scale),
        GetViewportState(device, regs, 6, scale),  GetViewportState(device, regs, 7, scale),
        GetViewportState(device, regs, 8, scale),  GetViewportState(device, regs, 9, scale),
        GetViewportState(device, regs, 10, scale), GetViewportState(device, regs, 11, scale),
        GetViewportState(device, regs, 12, scale), GetViewportState(device, regs, 13, scale),
        GetViewportState(device, regs, 14, scale), GetViewportState(device, regs, 15, scale),
    };
    // The host's jitter for NVIDIA DLSS (NoteCameraMotionDraw), in host pixels; added after the
    // scaling so it isn't rounded away.
    if (camera_motion.applied[0] != 0.0f || camera_motion.applied[1] != 0.0f) {
        for (VkViewport& viewport : viewport_list) {
            viewport.x += camera_motion.applied[0];
            viewport.y += camera_motion.applied[1];
        }
    }
    scheduler.Record([this, viewport_list](vk::CommandBuffer cmdbuf) {
        const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
        cmdbuf.SetViewport(0, viewports);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchScissors()) {
        return;
    }
    if (!regs.viewport_scale_offset_enabled) {
        u32 x = regs.surface_clip.x;
        u32 y = regs.surface_clip.y;
        u32 width = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
        u32 height = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y = regs.surface_clip.height - (y + height);
        }
        VkRect2D scissor{};
        scissor.offset.x = static_cast<int32_t>(x);
        scissor.offset.y = static_cast<int32_t>(y);
        scissor.extent.width  = width;
        scissor.extent.height = height;
        scheduler.Record([this, scissor](vk::CommandBuffer cmdbuf) {
            const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkRect2D, Maxwell::NumViewports> scissor_list{};
            scissor_list.fill(scissor);
            const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
            cmdbuf.SetScissor(0, scissors);
        });
        return;
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    const std::array scissor_list{
        GetScissorState(regs, 0, up_scale, down_shift),
        GetScissorState(regs, 1, up_scale, down_shift),
        GetScissorState(regs, 2, up_scale, down_shift),
        GetScissorState(regs, 3, up_scale, down_shift),
        GetScissorState(regs, 4, up_scale, down_shift),
        GetScissorState(regs, 5, up_scale, down_shift),
        GetScissorState(regs, 6, up_scale, down_shift),
        GetScissorState(regs, 7, up_scale, down_shift),
        GetScissorState(regs, 8, up_scale, down_shift),
        GetScissorState(regs, 9, up_scale, down_shift),
        GetScissorState(regs, 10, up_scale, down_shift),
        GetScissorState(regs, 11, up_scale, down_shift),
        GetScissorState(regs, 12, up_scale, down_shift),
        GetScissorState(regs, 13, up_scale, down_shift),
        GetScissorState(regs, 14, up_scale, down_shift),
        GetScissorState(regs, 15, up_scale, down_shift),
    };
    scheduler.Record([this, scissor_list](vk::CommandBuffer cmdbuf) {
        const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
        cmdbuf.SetScissor(0, scissors);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBias()) {
        return;
    }
    float units = regs.depth_bias / 2.0f;
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;

    if (is_d24 && !device.SupportsD24DepthBuffer()) {
        static constexpr const size_t length = sizeof(NEEDS_D24) / sizeof(NEEDS_D24[0]);

        static constexpr const u64* start = NEEDS_D24;
        static constexpr const u64* end = NEEDS_D24 + length;

        const u64* it = std::find(start, end, program_id);

        if (it != end) {
            // the base formulas can be obtained from here:
            //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
            const double rescale_factor =
                static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
            units = static_cast<float>(static_cast<double>(units) * rescale_factor);
        }
    }

    scheduler.Record([constant = units, clamp = regs.depth_bias_clamp,
                      factor = regs.slope_scale_depth_bias, this](vk::CommandBuffer cmdbuf) {
        if (device.IsExtDepthBiasControlSupported()) {
            static VkDepthBiasRepresentationInfoEXT bias_info{
                .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
                .pNext = nullptr,
                .depthBiasRepresentation =
                    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
                .depthBiasExact = VK_FALSE,
            };

            cmdbuf.SetDepthBias(constant, clamp, factor, &bias_info);
        } else {
            cmdbuf.SetDepthBias(constant, clamp, factor);
        }
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlendConstants()) {
        return;
    }
    const std::array blend_color = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                    regs.blend_color.a};
    scheduler.Record(
        [blend_color](vk::CommandBuffer cmdbuf) { cmdbuf.SetBlendConstants(blend_color.data()); });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBounds()) {
        return;
    }
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBounds(min, max); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilProperties()) {
        return;
    }
    bool update_references = state_tracker.TouchStencilReference();
    bool update_write_mask = state_tracker.TouchStencilWriteMask();
    bool update_compare_masks = state_tracker.TouchStencilCompare();
    if (state_tracker.TouchStencilSide(regs.stencil_two_side_enable != 0)) {
        update_references = true;
        update_write_mask = true;
        update_compare_masks = true;
    }
    if (update_references) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_back_ref);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_front_ref);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_ref = regs.stencil_front_ref, back_ref = regs.stencil_back_ref,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_ref != back_ref;
                // Front face
                cmdbuf.SetStencilReference(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_ref);
                if (set_back) {
                    cmdbuf.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, back_ref);
                }
            });
        }();
    }
    if (update_write_mask) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_back_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_front_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_write_mask = regs.stencil_front_mask,
                              back_write_mask = regs.stencil_back_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_write_mask != back_write_mask;
                // Front face
                cmdbuf.SetStencilWriteMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_write_mask);
                if (set_back) {
                    cmdbuf.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, back_write_mask);
                }
            });
        }();
    }
    if (update_compare_masks) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_back_func_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_front_func_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_test_mask = regs.stencil_front_func_mask,
                              back_test_mask = regs.stencil_back_func_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_test_mask != back_test_mask;
                // Front face
                cmdbuf.SetStencilCompareMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                      : VK_STENCIL_FACE_FRONT_AND_BACK,
                                             front_test_mask);
                if (set_back) {
                    cmdbuf.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, back_test_mask);
                }
            });
        }();
    }
    state_tracker.ClearStencilReset();
}

void RasterizerVulkan::UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineWidth()) {
        return;
    }
    const float width =
        regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
    scheduler.Record([width](vk::CommandBuffer cmdbuf) { cmdbuf.SetLineWidth(width); });
}

void RasterizerVulkan::UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchCullMode()) {
        return;
    }
    scheduler.Record([enabled = regs.gl_cull_test_enabled,
                      cull_face = regs.gl_cull_face](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetCullModeEXT(enabled ? MaxwellToVK::CullFace(cull_face) : VK_CULL_MODE_NONE);
    });
}

void RasterizerVulkan::UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBoundsTestEnable()) {
        return;
    }
    bool enabled = regs.depth_bounds_enable;
    if (enabled && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
        enabled = false;
    }
    scheduler.Record([enable = enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBoundsTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthWriteEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_write_enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthWriteEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchPrimitiveRestartEnable()) {
        return;
    }

    bool enable = regs.primitive_restart.enabled != 0;
    if (device.IsMoltenVK()) {
        enable = true;
    } else if (enable) {
        const auto topology = MaxwellToVK::PrimitiveTopology(device, maxwell3d->draw_manager.draw_state.topology);
        enable = IsPrimitiveRestartSupported(device, topology);
    }

    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetPrimitiveRestartEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchRasterizerDiscardEnable()) {
        return;
    }
    scheduler.Record([disable = regs.rasterize_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetRasterizerDiscardEnableEXT(disable == 0);
    });
}

void RasterizerVulkan::UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchConservativeRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3ConservativeRasterizationMode()) {
        return;
    }

    scheduler.Record([enable = regs.conservative_raster_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetConservativeRasterizationModeEXT(
            enable ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                   : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
    });
}

void RasterizerVulkan::UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineStippleEnable()) {
        return;
    }

    if (!device.SupportsDynamicState3LineStippleEnable()) {
        return;
    }

    scheduler.Record([enable = regs.line_stipple_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineStippleEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!device.IsExtLineRasterizationSupported()) {
        return;
    }
    if (!state_tracker.TouchLineRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3LineRasterizationMode()) {
        static std::once_flag warn_missing_rect;
        std::call_once(warn_missing_rect, [] {
            LOG_WARNING(Render_Vulkan,
                        "Driver lacks rectangular line rasterization support; skipping dynamic "
                        "line state updates");
        });
        return;
    }

    const bool wants_smooth = regs.line_anti_alias_enable != 0;
    VkLineRasterizationModeEXT mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
    if (wants_smooth) {
        if (device.SupportsSmoothLines()) {
            mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        } else {
            static std::once_flag warn_missing_smooth;
            std::call_once(warn_missing_smooth, [] {
                LOG_WARNING(Render_Vulkan,
                            "Line anti-aliasing requested but smoothLines feature unavailable; "
                            "using rectangular rasterization");
            });
        }
    }
    scheduler.Record([mode](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineRasterizationModeEXT(mode);
    });
}

void RasterizerVulkan::UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBiasEnable()) {
        return;
    }
    constexpr size_t POINT = 0;
    constexpr size_t LINE = 1;
    constexpr size_t POLYGON = 2;
    static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
        POINT,   // Points
        LINE,    // Lines
        LINE,    // LineLoop
        LINE,    // LineStrip
        POLYGON, // Triangles
        POLYGON, // TriangleStrip
        POLYGON, // TriangleFan
        POLYGON, // Quads
        POLYGON, // QuadStrip
        POLYGON, // Polygon
        LINE,    // LinesAdjacency
        LINE,    // LineStripAdjacency
        POLYGON, // TrianglesAdjacency
        POLYGON, // TriangleStripAdjacency
        POLYGON, // Patches
    };
    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = u32(maxwell3d->draw_manager.draw_state.topology);
    const u32 enable = enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]];
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBiasEnableEXT(enable != 0); });
}

void RasterizerVulkan::UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOpEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3LogicOpEnable()) {
        return;
    }
    scheduler.Record([enable = regs.logic_op.enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLogicOpEnableEXT(enable != 0);
    });
}

void RasterizerVulkan::UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthClampEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3DepthClampEnable()) {
        return;
    }
    bool is_enabled = !(regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
    scheduler.Record(
        [is_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthClampEnableEXT(is_enabled); });
}

void RasterizerVulkan::UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchAlphaToCoverageEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToCoverageEnable()) {
        return;
    }
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToCoverage() &&
                        regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToCoverageEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchAlphaToOneEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToOneEnable()) {
        static std::once_flag warn_alpha_to_one;
        std::call_once(warn_alpha_to_one, [] {
            LOG_WARNING(Render_Vulkan,
                        "Alpha-to-one is not supported on this device; forcing it disabled");
        });
        return;
    }
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToOne() &&
                        regs.anti_alias_alpha_control.alpha_to_one != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToOneEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthCompareOp()) {
        return;
    }
    scheduler.Record([func = regs.depth_test_func](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthCompareOpEXT(MaxwellToVK::ComparisonOp(func));
    });
}

void RasterizerVulkan::UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchFrontFace()) {
        return;
    }

    VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    scheduler.Record(
        [front_face](vk::CommandBuffer cmdbuf) { cmdbuf.SetFrontFaceEXT(front_face); });
}

void RasterizerVulkan::UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilOp()) {
        return;
    }
    const Maxwell::StencilOp::Op fail = regs.stencil_front_op.fail;
    const Maxwell::StencilOp::Op zfail = regs.stencil_front_op.zfail;
    const Maxwell::StencilOp::Op zpass = regs.stencil_front_op.zpass;
    const Maxwell::ComparisonOp compare = regs.stencil_front_op.func;
    if (regs.stencil_two_side_enable) {
        // Separate stencil op per face
        const Maxwell::StencilOp::Op back_fail = regs.stencil_back_op.fail;
        const Maxwell::StencilOp::Op back_zfail = regs.stencil_back_op.zfail;
        const Maxwell::StencilOp::Op back_zpass = regs.stencil_back_op.zpass;
        const Maxwell::ComparisonOp back_compare = regs.stencil_back_op.func;
        scheduler.Record([fail, zfail, zpass, compare, back_fail, back_zfail, back_zpass,
                          back_compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_BIT, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, MaxwellToVK::StencilOp(back_fail),
                                   MaxwellToVK::StencilOp(back_zpass),
                                   MaxwellToVK::StencilOp(back_zfail),
                                   MaxwellToVK::ComparisonOp(back_compare));
        });
    } else {
        // Front face defines the stencil op of both faces
        scheduler.Record([fail, zfail, zpass, compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_AND_BACK, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
        });
    }
}

void RasterizerVulkan::UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOp()) {
        return;
    }
    const auto op_value = static_cast<u32>(regs.logic_op.op);
    auto op = op_value >= 0x1500 && op_value < 0x1510 ? static_cast<VkLogicOp>(op_value - 0x1500)
                                                      : VK_LOGIC_OP_NO_OP;
    scheduler.Record([op](vk::CommandBuffer cmdbuf) { cmdbuf.SetLogicOpEXT(op); });
}

void RasterizerVulkan::UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs,
                                      const GraphicsPipeline* pipeline) {
    // The write mask also excludes components the fragment shader doesn't output, so it has to be
    // recorded again whenever the bound pipeline outputs a different set of components.
    const u32 color_output_mask = pipeline ? pipeline->ColorOutputMask() : ~0U;
    const bool color_output_changed = color_output_mask != last_color_output_mask;
    if (!state_tracker.TouchBlending() && !color_output_changed) {
        return;
    }

    if (state_tracker.TouchColorMask() || color_output_changed) {
        last_color_output_mask = color_output_mask;
        std::array<VkColorComponentFlags, Maxwell::NumRenderTargets> setup_masks{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            auto& current = setup_masks[index];
            if (mask.R) {
                current |= VK_COLOR_COMPONENT_R_BIT;
            }
            if (mask.G) {
                current |= VK_COLOR_COMPONENT_G_BIT;
            }
            if (mask.B) {
                current |= VK_COLOR_COMPONENT_B_BIT;
            }
            if (mask.A) {
                current |= VK_COLOR_COMPONENT_A_BIT;
            }
            current &= (color_output_mask >> (index * 4)) & 0xF;
        }
        scheduler.Record([setup_masks](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorWriteMaskEXT(0, setup_masks);
        });
    }

    if (state_tracker.TouchBlendEnable()) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            bool is_integer = false;
            if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                is_integer = IsPixelFormatInteger(format);
            }
            setup_enables[index] =
                (!is_integer && regs.blend.enable[index] != 0) ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
    }

    if (state_tracker.TouchBlendEquations()) {
        std::array<VkColorBlendEquationEXT, Maxwell::NumRenderTargets> setup_blends{};

        const auto blend_setup = [&](auto& host_blend, const auto& guest_blend) {
            host_blend.srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source);
            host_blend.dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest);
            host_blend.colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op);
            host_blend.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source);
            host_blend.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest);
            host_blend.alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op);
        };

        // Single blend equation for all targets
        if (!regs.blend_per_target_enabled) {
            // Temporary workaround for games that use iterated blending
            if (regs.iterated_blend.enable && Settings::values.use_squashed_iterated_blend) {
                setup_blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].colorBlendOp = VK_BLEND_OP_ADD;
                setup_blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                setup_blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                setup_blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                blend_setup(setup_blends[0], regs.blend);
            }

            // Copy first blend state to all other targets
            for (size_t index = 1; index < Maxwell::NumRenderTargets; index++) {
                setup_blends[index] = setup_blends[0];
            }
        } else {
            // Per-target blending
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                blend_setup(setup_blends[index], regs.blend_per_target[index]);
            }
        }

        scheduler.Record([setup_blends](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEquationEXT(0, setup_blends);
        });
    }
}

void RasterizerVulkan::UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchColorMask()) {
        return;
    }
    std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        setup_enables[index] = (mask.R || mask.G || mask.B || mask.A) ? VK_TRUE : VK_FALSE;
    }
    scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetColorWriteEnableEXT(setup_enables);
    });
}

void RasterizerVulkan::UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.stencil_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetStencilTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    auto& dirty{maxwell3d->dirty.flags};
    const bool vertex_input_dirty = dirty[Dirty::VertexInput];
    const bool vertex_buffers_dirty = dirty[VideoCommon::Dirty::VertexBuffers];
    if (!vertex_input_dirty && !vertex_buffers_dirty) {
        return;
    }
    dirty[Dirty::VertexInput] = false;

    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;

    const u32 max_attributes =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexAttributes,
                                          device.GetMaxVertexInputAttributes()));
    const u32 max_bindings =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexArrays,
                                          device.GetMaxVertexInputBindings()));


    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset = attribute.offset +
                      (buffer_cache_runtime.IsVertexRepacked(binding) && attribute.offset >= 65 ? 3 : 0),
        });
    }

    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride = buffer_cache_runtime.IsVertexRepacked(binding) ? 72U : input_binding.stride.Value(),
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }

    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        dirty[Dirty::VertexAttribute0 + index] = false;
    }
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        dirty[Dirty::VertexBinding0 + index] = false;
    }

    scheduler.Record([bindings, attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetVertexInputEXT(bindings, attributes);
    });
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
    bound_channel_id = channel_id;
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    if (channel_id == bound_channel_id) {
        state_tracker.ClearChannel();
    }
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

} // namespace Vulkan
