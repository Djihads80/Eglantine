// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fmt/ranges.h>

#include "common/cityhash.h"
#include "common/range_sets.inc"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/hot_pages.h"
#include "video_core/buffer_cache/uniform_stream_reuse.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_quick_bind.h"
#include "video_core/nx2_speculate.h"
#include "video_core/nx2_strato.h"
#include "video_core/texture_cache/util.h"

namespace VideoCommon {

using Core::DEVICE_PAGESIZE;

template <class P>
BufferCache<P>::BufferCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, Runtime& runtime_)
    : runtime{runtime_}, device_memory{device_memory_}, memory_tracker{device_memory} {
    // A new runtime has a new scheduler (its ticks restart) and a new stream buffer: whatever the
    // stream uniform reuse remembers refers to the old ones. Before the early return below.
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    // Ensure the first slot is used for the null buffer
    void(slot_buffers.insert(runtime, NullBufferParams{}));
    gpu_modified_ranges.Clear();
    inline_buffer_id = NULL_BUFFER_ID;
#ifdef YUZU_LEGACY
    immediately_free = (Settings::values.vram_usage_mode.GetValue() == Settings::VramUsageMode::Aggressive);
#endif
    if (!runtime.CanReportMemoryUsage()) {
        minimum_memory = DEFAULT_EXPECTED_MEMORY;
        critical_memory = DEFAULT_CRITICAL_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
    const s64 min_spacing_expected = device_local_memory - 1_GiB;
    const s64 min_spacing_critical = device_local_memory - 512_MiB;
    const s64 mem_threshold = (std::min)(device_local_memory, TARGET_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    minimum_memory = static_cast<u64>(
        (std::max)((std::min)(device_local_memory - min_vacancy_expected, min_spacing_expected),
                 DEFAULT_EXPECTED_MEMORY));
    critical_memory = static_cast<u64>(
        (std::max)((std::min)(device_local_memory - min_vacancy_critical, min_spacing_critical),
                 DEFAULT_CRITICAL_MEMORY));
}

template <class P>
BufferCache<P>::~BufferCache() = default;

template <class P>
void BufferCache<P>::RunGarbageCollector(bool aggressive_gc) {
    const u64 ticks_to_destroy = aggressive_gc ? 60 : 120;
    int num_iterations = aggressive_gc ? 64 : 32;
    // Their GPU-written data is downloaded first, all with one wait for the GPU: one wait per
    // buffer stalled the GPU thread every frame on title 1 (an 8 GB GPU near its budget, no
    // per-fence downloads keeping the buffers clean), ~2.5% of its time.
    boost::container::small_vector<BufferId, 64> evicted;
    lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, [&](BufferId buffer_id) {
        if (num_iterations == 0) {
            return true;
        }
        --num_iterations;
        // Below the critical threshold, buffers holding GPU-written data stay: evicting one
        // means downloading it first. The thresholds compare the whole device's use (mostly
        // textures) with fixed headroom, so on an 8 GB GPU the collector ran every frame once a
        // title 1 course was loaded, and the downloads were ~2.7% of the GPU thread.
        const Buffer& buffer = slot_buffers[buffer_id];
        if (!aggressive_gc && gpu_modified_ranges.Intersects(buffer.CpuAddr(), buffer.SizeBytes())) {
            return false;
        }
        evicted.push_back(buffer_id);
        return false;
    });
    if constexpr (USE_MEMORY_MAPS) {
        DownloadBuffersMemory(evicted);
    } else {
        for (const BufferId buffer_id : evicted) {
            DownloadBufferMemory(slot_buffers[buffer_id]);
        }
    }
    for (const BufferId buffer_id : evicted) {
        DeleteBuffer(buffer_id);
    }
}

template <class P>
void BufferCache<P>::TickFrame() {
    // Homebrew console apps don't create or bind any channels, so this will be nullptr.
    if (!channel_state) {
        return;
    }
    runtime.TickFrame(slot_buffers);
    native_indirect_uniforms_this_frame.clear();
    if (HotPages::Enabled()) {
        TickHotPages();
    }
    if (frame_tick % 600 == 0 && !uploaded_buffer_ranges.empty()) {
        for (const auto& [address, size] : uploaded_buffer_ranges) {
            device_memory.HintCold(address, size);
        }
        uploaded_buffer_ranges.clear();
    }
    if (static u64 frames{}; ++frames % 600 == 0) {
        u64 total{};
        size_t count{};
        std::vector<std::pair<u64, VAddr>> largest;
        for (const auto [buffer_id, buffer] : slot_buffers) {
            total += buffer->SizeBytes();
            ++count;
            largest.emplace_back(buffer->SizeBytes(), buffer->CpuAddr());
        }
        std::partial_sort(largest.begin(), largest.begin() + std::min<size_t>(6, largest.size()),
                          largest.end(), std::greater<>{});
        LOG_INFO(HW_GPU, "Buffer cache: {} buffers {} MiB; largest {}", count, total >> 20,
                 fmt::join(largest.begin(), largest.begin() + std::min<size_t>(6, largest.size()), ", "));
    }

    // Calculate hits and shots and move hit bits to the right
    const u32 hits = std::reduce(channel_state->uniform_cache_hits.begin(),
                                 channel_state->uniform_cache_hits.end());
    const u32 shots = std::reduce(channel_state->uniform_cache_shots.begin(),
                                  channel_state->uniform_cache_shots.end());
    std::copy_n(channel_state->uniform_cache_hits.begin(),
                channel_state->uniform_cache_hits.size() - 1,
                channel_state->uniform_cache_hits.begin() + 1);
    std::copy_n(channel_state->uniform_cache_shots.begin(),
                channel_state->uniform_cache_shots.size() - 1,
                channel_state->uniform_cache_shots.begin() + 1);
    channel_state->uniform_cache_hits[0] = 0;
    channel_state->uniform_cache_shots[0] = 0;

    const bool skip_preferred = hits * 256 < shots * 251;
    channel_state->uniform_buffer_skip_cache_size = skip_preferred ? DEFAULT_SKIP_CACHE_SIZE : 0;

    // If we can obtain the memory info, use it instead of the estimate.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
    }
    if (gpu_memory != nullptr && gpu_memory->UsesNativeTiling()) {
        // Switch 2 titles fill the device with textures (their collector watches the budget);
        // title 1's buffers were ~200 MiB of an 8 GB GPU's ~6.8 GB in use, yet this collector
        // ran aggressively every frame on the device-wide figure, evicting up to 64 buffers (and
        // downloading the GPU-written ones, ~5% of the GPU thread) that were recreated later.
        // Collect when the buffers themselves are large. At 1 GiB it held geometry of scenes
        // long gone: title 1's title screen grew to ~0.9 GB of buffers in ten minutes.
        if (buffer_bytes >= 512_MiB) {
            RunGarbageCollector(buffer_bytes >= 1_GiB);
        }
    } else if (total_used_memory >= minimum_memory) {
        RunGarbageCollector(total_used_memory >= critical_memory);
    }
    ++frame_tick;
    delayed_destruction_ring.Tick();

    for (auto& buffer : async_buffers_death_ring) {
        runtime.FreeDeferredStagingBuffer(buffer);
    }
    async_buffers_death_ring.clear();
}

template <class P>
void BufferCache<P>::UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size) {
    if constexpr (requires { runtime.BindMultiRangeStorageBuffer(u64{}, bool{}); }) {
        virtual_ranges.Unmap(as_id, gpu_addr, size);
    }
}

template <class P>
void BufferCache<P>::WriteMemory(DAddr device_addr, u64 size, bool keep_page_gpu_data) {
    Nx2NoteListHostWrite(device_addr, size);
    if (IsRegionGpuModified(device_addr, size)) {
        ClearDownload(device_addr, size);
        gpu_modified_ranges.Subtract(device_addr, size);
    }
    // The tracker marks whole pages CPU-modified, which drops their GPU-modified state: GPU-written
    // bytes elsewhere on these pages would never be downloaded, and the page's next upload would
    // send the stale guest copy over them. Hyrule Warriors: Age of Imprisonment writes small
    // structures next to the draw arguments its compute dispatches fill in; the draws then read
    // leftover bytes as vertex counts. Download those bytes first (the CPU's own were subtracted
    // above, so they aren't overwritten).
    // Only the scheduler's owner may download (it records and submits): unmaps and other writes
    // reach this from CPU and service threads too, and submitting there raced the GPU thread's
    // own submissions (crashes in Scheduler::SubmitExecution). Those keep the old behaviour;
    // guest CPU writes come through TryCacheInvalidation, which sends such pages to the GPU thread.
    const DAddr page_begin = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
    const DAddr page_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
    if (keep_page_gpu_data && VideoCommon::GpuSplit::OwnsScheduler() &&
        gpu_modified_ranges.Intersects(page_begin, page_end - page_begin)) {
        // Switch 2 titles: upload the CPU's bytes into the buffer instead, as CachedWriteMemory
        // does, so the page stays GPU-owned and nothing waits for the host GPU.
        static const int forced = [] {
            const char* value = std::getenv("EDEN_NX2_INLINE_PAGE_WRITES");
            return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
        }();
        const bool inline_writes =
            forced >= 0 ? forced == 1 : Tegra::g_nx2_ampere.load(std::memory_order_relaxed);
        if (inline_writes && IsRegionRegistered(device_addr, size)) {
            tmp_buffer.resize_destructive(size);
            device_memory.ReadBlockUnsafe(device_addr, tmp_buffer.data(), size);
            InlineMemoryImplementation(device_addr, size, tmp_buffer);
            return;
        }
        DownloadMemory(page_begin, page_end - page_begin);
    }
    memory_tracker.MarkRegionAsCpuModified(device_addr, size, true);
    MarkVertexBuffersStale(device_addr, size);
}

template <class P>
void BufferCache<P>::CachedWriteMemory(DAddr device_addr, u64 size) {
    const bool is_dirty = IsRegionRegistered(device_addr, size);
    if (!is_dirty) {
        return;
    }
    DAddr aligned_start = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
    DAddr aligned_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
    if (!IsRegionGpuModified(aligned_start, aligned_end - aligned_start)) {
        WriteMemory(device_addr, size);
        return;
    }

    Nx2NoteListHostWrite(device_addr, size);
    tmp_buffer.resize_destructive(size);
    device_memory.ReadBlockUnsafe(device_addr, tmp_buffer.data(), size);

    InlineMemoryImplementation(device_addr, size, tmp_buffer);
}

template <class P>
void BufferCache<P>::ForgetGpuWrite(DAddr device_addr, u64 size) {
    if (!gpu_modified_ranges.Intersects(device_addr, size)) {
        return;
    }
    ClearDownload(device_addr, size);
    gpu_modified_ranges.Subtract(device_addr, size);
}

template <class P>
bool BufferCache<P>::OnCPUWrite(DAddr device_addr, u64 size) {
    const bool is_dirty = IsRegionRegistered(device_addr, size);
    if (!is_dirty) {
        return false;
    }
    if (memory_tracker.IsRegionGpuModified(device_addr, size)) {
        return true;
    }
    WriteMemory(device_addr, size);
    return false;
}

template <class P>
std::optional<VideoCore::RasterizerDownloadArea> BufferCache<P>::GetFlushArea(DAddr device_addr,
                                                                              u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> area{};
    area.emplace();
    DAddr device_addr_start_aligned = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
    DAddr device_addr_end_aligned = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
    area->start_address = device_addr_start_aligned;
    area->end_address = device_addr_end_aligned;
    if (memory_tracker.IsRegionPreflushable(device_addr, size)) {
        area->preemtive = true;
        return area;
    };
    area->preemtive = !IsRegionGpuModified(device_addr_start_aligned,
                                           device_addr_end_aligned - device_addr_start_aligned);
    // Mark only data that the GPU wrote. A mark stays for the session and sends later GPU writes
    // in the range to a download at each fence.
    if (!area->preemtive && !ReadTrapFlushes()) {
        memory_tracker.MarkRegionAsPreflushable(device_addr_start_aligned,
                                                device_addr_end_aligned - device_addr_start_aligned);
    } else if (!area->preemtive) {
        // Trap and flush instead: no mark, so the next GPU write here arms the trap again. The
        // flush that follows also downloads the GPU writes in every range read before.
        if constexpr (USE_MEMORY_MAPS) {
            cpu_read_ranges.Add(area->start_address, area->end_address - area->start_address);
            download_cpu_read_ranges = true;
        }
        // The core consumed this trap before asking: forget it, or ArmReadTraps would skip
        // rearming it at the next GPU write.
        read_trapped_ranges.Subtract(device_addr_start_aligned,
                                     device_addr_end_aligned - device_addr_start_aligned);
    } else {
        // The core keeps this area and has no mark. The next GPU write here must arm the range
        // again, which ends the area and sets the trap.
        read_trapped_ranges.Subtract(device_addr_start_aligned,
                                     device_addr_end_aligned - device_addr_start_aligned);
    }
    return area;
}

template <class P>
void BufferCache<P>::DownloadMemory(DAddr device_addr, u64 size) {
    // EDEN_NX2_DOWNLOAD_WINDOW=1 also takes the pages around the range, below. Off: even with
    // only whole, protected pages of the buffer, Mario Kart World faulted the GPU within two
    // minutes (CP opcode errors), and raced 12 minutes without one with it off. Why is not found.
    static const bool window = [] {
        const char* value = std::getenv("EDEN_NX2_DOWNLOAD_WINDOW");
        return value != nullptr && value[0] != '0';
    }();
    if constexpr (USE_MEMORY_MAPS) {
        // EDEN_NX2_READ_TRAP_FLUSH: a guest CPU read trapped (GetFlushArea). Ocarina of Time
        // reads the output of ~30 compute storage buffers each frame page by page: download the
        // GPU writes in every range read so far with this one, in one GPU wait where no other
        // thread's batch is open, and the reads after it find nothing to flush.
        if (download_cpu_read_ranges) {
            download_cpu_read_ranges = false;
            boost::container::small_vector<std::pair<DAddr, u64>, 16> ranges{{device_addr, size}};
            cpu_read_ranges.ForEach([&](DAddr lower, DAddr upper) {
                gpu_modified_ranges.ForEachInRange(lower, upper - lower,
                                                   [&](DAddr start, DAddr end) {
                                                       ranges.emplace_back(start, end - start);
                                                   });
            });
            const bool own_batch = !IS_OPENGL && !download_batch;
            if (own_batch) {
                BeginDownloadBatch();
            }
            for (const std::pair<DAddr, u64>& range : ranges) {
                ForEachBufferInRange(range.first, range.second, [&](BufferId, Buffer& buffer) {
                    DownloadBufferMemory(buffer, range.first, range.second);
                });
            }
            if (own_batch) {
                EndDownloadBatch();
            }
            return;
        }
    }
    ForEachBufferInRange(device_addr, size, [&](BufferId, Buffer& buffer) {
        if (!window) {
            DownloadBufferMemory(buffer, device_addr, size);
            return;
        }
        // Each download waits for the whole GPU, and Mario Kart World's macros and CPU read
        // GPU-written data a few words at a time: also take the GPU-written pages of the same
        // buffer in the 64 KiB around the range, so the reads after them find it flushed.
        // Only whole pages of this buffer: the tracking is per page, and clearing a page this
        // buffer shares with another would drop the other's GPU-written state and copy bytes
        // from outside this buffer (taking every page in the window corrupted guest memory
        // and faulted the GPU within minutes). And only pages still write-protected: a CPU write
        // there faults and flushes first, so the GPU's data is the newest.
        constexpr u64 Window = u64{64} << 10;
        constexpr u64 Page = Core::DEVICE_PAGESIZE;
        const DAddr owned_begin = Common::AlignUp(buffer.CpuAddr(), Page);
        const DAddr owned_end = Common::AlignDown(buffer.CpuAddr() + buffer.SizeBytes(), Page);
        const DAddr begin = (std::max<DAddr>)(Common::AlignDown(device_addr, Window), owned_begin);
        const DAddr end = (std::min<DAddr>)(Common::AlignUp(device_addr + size, Window), owned_end);
        const DAddr asked_begin = Common::AlignDown(device_addr, Page);
        const DAddr asked_end = Common::AlignUp(device_addr + size, Page);
        boost::container::small_vector<std::pair<DAddr, u64>, 8> ranges{{device_addr, size}};
        for (DAddr page = begin; page < end; page += Page) {
            if ((page >= asked_begin && page < asked_end) ||
                !memory_tracker.IsRegionGpuModified(page, Page) ||
                memory_tracker.IsRegionUntracked(page, Page)) {
                continue;
            }
            if (ranges.size() > 1 && ranges.back().first + ranges.back().second == page) {
                ranges.back().second += Page;
            } else {
                ranges.emplace_back(page, Page);
            }
        }
        DownloadBufferRanges(buffer, ranges);
    });
}

template <class P>
void BufferCache<P>::ClearDownload(DAddr device_addr, u64 size) {
    for (auto& ranges : pending_download_ranges) {
        ranges.Subtract(device_addr, size);
    }
    uncommitted_gpu_modified_ranges.Subtract(device_addr, size);
    for (auto& interval_set : committed_gpu_modified_ranges) {
        interval_set.Subtract(device_addr, size);
    }
}

template <class P>
bool BufferCache<P>::DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    const std::optional<DAddr> cpu_src_address = gpu_memory->GpuToCpuAddress(src_address);
    const std::optional<DAddr> cpu_dest_address = gpu_memory->GpuToCpuAddress(dest_address);
    if (!cpu_src_address || !cpu_dest_address) {
        return false;
    }
    const bool source_dirty = IsRegionRegistered(*cpu_src_address, amount);
    const bool dest_dirty = IsRegionRegistered(*cpu_dest_address, amount);
    if (!source_dirty && !dest_dirty) {
        return false;
    }

    ClearDownload(*cpu_dest_address, amount);
    MarkVertexBuffersStale(*cpu_dest_address, amount);

    BufferId buffer_a;
    BufferId buffer_b;
    do {
        channel_state->has_deleted_buffers = false;
        buffer_a = FindBuffer(*cpu_src_address, static_cast<u32>(amount), false);
        buffer_b = FindBuffer(*cpu_dest_address, static_cast<u32>(amount), false);
    } while (channel_state->has_deleted_buffers);
    auto& src_buffer = slot_buffers[buffer_a];
    auto& dest_buffer = slot_buffers[buffer_b];
    SynchronizeBuffer(src_buffer, *cpu_src_address, static_cast<u32>(amount));
    SynchronizeBuffer(dest_buffer, *cpu_dest_address, static_cast<u32>(amount));
    std::array copies{BufferCopy{
        .src_offset = src_buffer.Offset(*cpu_src_address),
        .dst_offset = dest_buffer.Offset(*cpu_dest_address),
        .size = amount,
    }};

    boost::container::small_vector<std::pair<DAddr, size_t>, 4> tmp_intervals;
    auto mirror = [&](DAddr base_address, DAddr base_address_end) {
        const u64 size = base_address_end - base_address;
        const DAddr diff = base_address - *cpu_src_address;
        const DAddr new_base_address = *cpu_dest_address + diff;
        tmp_intervals.push_back({new_base_address, size});
        QueueFenceDownload(new_base_address, size);
    };
    gpu_modified_ranges.ForEachInRange(*cpu_src_address, amount, mirror);
    // This subtraction in this order is important for overlapping copies.
    gpu_modified_ranges.Subtract(*cpu_dest_address, amount);
    const bool has_new_downloads = tmp_intervals.size() != 0;
    for (const auto& pair : tmp_intervals) {
        gpu_modified_ranges.Add(pair.first, pair.second);
        if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
            Tegra::Nx2MarkGpuWritten(pair.first, pair.second);
            Nx2NoteListWrite(buffer_b, pair.first, pair.second);
        }
    }
    const auto& copy = copies[0];
    src_buffer.MarkUsage(copy.src_offset, copy.size);
    dest_buffer.MarkUsage(copy.dst_offset, copy.size);
    NoteGpuWrite(dest_buffer, *cpu_dest_address, amount);
    runtime.CopyBuffer(dest_buffer, src_buffer, copies, true);
    if (Tegra::Nx2ClSpeculate()) {
        Tegra::Nx2SetRangeSig(*cpu_dest_address, amount,
                              Tegra::Nx2SigMix(0x646d61ULL, Tegra::Nx2RangeSig(*cpu_src_address,
                                                                               amount)));
    }
    if (has_new_downloads) {
        memory_tracker.MarkRegionAsGpuModified(*cpu_dest_address, amount);
        const bool should_sync = Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
        if (should_sync) {
            runtime.Finish();
        }
    }

    Tegra::Memory::DeviceGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::UnsafeReadWrite>
        tmp(device_memory, *cpu_src_address, amount, &tmp_buffer);
    tmp.SetAddressAndSize(*cpu_dest_address, amount);
    return true;
}

template <class P>
bool BufferCache<P>::DMAClear(GPUVAddr dst_address, u64 amount, u32 value) {
    const std::optional<DAddr> cpu_dst_address = gpu_memory->GpuToCpuAddress(dst_address);
    if (!cpu_dst_address) {
        return false;
    }
    const bool dest_dirty = IsRegionRegistered(*cpu_dst_address, amount);
    if (!dest_dirty) {
        return false;
    }

    const size_t size = amount * sizeof(u32);
    ClearDownload(*cpu_dst_address, size);
    gpu_modified_ranges.Subtract(*cpu_dst_address, size);

    const BufferId buffer = FindBuffer(*cpu_dst_address, static_cast<u32>(size), false);
    Buffer& dest_buffer = slot_buffers[buffer];
    const u32 offset = dest_buffer.Offset(*cpu_dst_address);
    NoteGpuWrite(dest_buffer, *cpu_dst_address, size);
    runtime.ClearBuffer(dest_buffer, offset, size, value);
    if (Tegra::Nx2ClSpeculate()) {
        Tegra::Nx2SetRangeSig(*cpu_dst_address, size, Tegra::Nx2SigMix(0x636c656172ULL, value));
    }
    dest_buffer.MarkUsage(offset, size);
    return true;
}

template <class P>
std::pair<typename P::Buffer*, u32> BufferCache<P>::ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                                 ObtainBufferSynchronize sync_info,
                                                                 ObtainBufferOperation post_op) {
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!device_addr) {
        return {&slot_buffers[NULL_BUFFER_ID], 0};
    }
    return ObtainCPUBuffer(*device_addr, size, sync_info, post_op);
}

template <class P>
std::pair<typename P::Buffer*, u32> BufferCache<P>::ObtainCPUBuffer(
    DAddr device_addr, u32 size, ObtainBufferSynchronize sync_info, ObtainBufferOperation post_op) {
    const BufferId buffer_id = FindBuffer(device_addr, size, false);
    Buffer& buffer = slot_buffers[buffer_id];

    // synchronize op
    switch (sync_info) {
    case ObtainBufferSynchronize::FullSynchronize:
        SynchronizeBuffer(buffer, device_addr, size);
        break;
    default:
        break;
    }

    switch (post_op) {
    case ObtainBufferOperation::MarkAsWritten:
        MarkWrittenBuffer(buffer_id, device_addr, size);
        break;
    case ObtainBufferOperation::NoteGpuWrite:
        NoteGpuWrite(slot_buffers[buffer_id], device_addr, size);
        if constexpr (!IS_OPENGL) {
            if (Tegra::Nx2ClSpeculate()) {
                Tegra::Nx2SetRangeUnique(device_addr, size, runtime.CurrentTick());
            }
        }
        break;
    case ObtainBufferOperation::DiscardWrite: {
        ClearDownload(device_addr, size);
        gpu_modified_ranges.Subtract(device_addr, size);
        break;
    }
    default:
        break;
    }

    return {&buffer, buffer.Offset(device_addr)};
}

template <class P>
void BufferCache<P>::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                               u32 size) {
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!device_addr) {
        channel_state->uniform_buffers[stage][index] = NULL_BINDING;
        return;
    }
    const Binding binding{
        .device_addr = *device_addr,
        .size = size,
        .buffer_id = BufferId{},
    };
    channel_state->uniform_buffers[stage][index] = binding;
}

template <class P>
void BufferCache<P>::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    auto& current = channel_state->uniform_buffers[stage][index];
    if (current.device_addr == 0 && current.size == 0 && current.buffer_id == NULL_BUFFER_ID)
        [[likely]] {
        return;
    }
    current = NULL_BINDING;
}

template <class P>
void BufferCache<P>::UpdateGraphicsBuffers(bool is_indexed) {
    do {
        channel_state->has_deleted_buffers = false;
        DoUpdateGraphicsBuffers(is_indexed);
    } while (channel_state->has_deleted_buffers);
}

template <class P>
void BufferCache<P>::UpdateComputeBuffers() {
    do {
        channel_state->has_deleted_buffers = false;
        DoUpdateComputeBuffers();
    } while (channel_state->has_deleted_buffers);
}

template <class P>
void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {
    if (is_indexed) {
        BindHostIndexBuffer();
    } else if constexpr (!HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        if (draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
            draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
            runtime.BindQuadIndexBuffer(draw_state.topology, draw_state.vertex_buffer.first,
                                        draw_state.vertex_buffer.count);
        }
    }
    BindHostVertexBuffers();
    BindHostTransformFeedbackBuffers();
    if (current_draw_indirect) {
        BindHostDrawIndirectBuffers();
    }
}

template <class P>
void BufferCache<P>::BindHostStageBuffers(size_t stage) {
    BindHostGraphicsUniformBuffers(stage);
    if (channel_state->enabled_storage_buffers[stage] != 0) {
        Nx2Profile::Scope scope{Nx2Profile::Zone::SsboBind};
        BindHostGraphicsStorageBuffers(stage);
    }
    if (channel_state->enabled_texture_buffers[stage] != 0) {
        Nx2Profile::Scope scope{Nx2Profile::Zone::TexBufBind};
        BindHostGraphicsTextureBuffers(stage);
    }
}

template <class P>
void BufferCache<P>::BindHostComputeBuffers() {
    BindHostComputeUniformBuffers();
    if (channel_state->enabled_compute_storage_buffers != 0) {
        BindHostComputeStorageBuffers();
    }
    if (channel_state->enabled_compute_texture_buffers != 0) {
        BindHostComputeTextureBuffers();
    }
    if (any_buffer_uploaded) {
        runtime.PostCopyBarrier();
        any_buffer_uploaded = false;
    }
}

template <class P>
void BufferCache<P>::SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
                                            const UniformBufferSizes* sizes) {
    if (channel_state->enabled_uniform_buffer_masks == mask &&
        channel_state->uniform_buffer_sizes == sizes) [[likely]] {
        return;
    }
    if (channel_state->enabled_uniform_buffer_masks != mask) {
        channel_state->fast_bound_uniform_buffers.fill(0);
        if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
            channel_state->dirty_uniform_buffers.fill(~u32{0});
            channel_state->uniform_buffer_binding_sizes.fill({});
        }
        channel_state->enabled_uniform_buffer_masks = mask;
    }
    channel_state->uniform_buffer_sizes = sizes;
}

template <class P>
void BufferCache<P>::SetComputeUniformBufferState(u32 mask,
                                                  const ComputeUniformBufferSizes* sizes) {
    channel_state->enabled_compute_uniform_buffer_mask = mask;
    channel_state->compute_uniform_buffer_sizes = sizes;
}

template <class P>
void BufferCache<P>::UnbindGraphicsStorageBuffers(size_t stage) {
    if (channel_state->enabled_storage_buffers[stage] == 0) {
        return;
    }
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers()) {
            channel_state->total_graphics_storage_buffers -=
                static_cast<u32>(std::popcount(channel_state->enabled_storage_buffers[stage]));
        }
    }
    channel_state->enabled_storage_buffers[stage] = 0;
    channel_state->written_storage_buffers[stage] = 0;
}

template <class P>
bool BufferCache<P>::BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index,
                                               u32 cbuf_offset, bool is_written,
                                               u32 indirect_offset) {
    if (stage >= NUM_STAGES || ssbo_index >= NUM_STORAGE_BUFFERS) [[unlikely]] {
        LOG_ERROR(HW_GPU, "Invalid graphics storage buffer binding: stage {}, index {}", stage,
                  ssbo_index);
        return false;
    }
    const bool already_enabled =
        ((channel_state->enabled_storage_buffers[stage] >> ssbo_index) & 1U) != 0;
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers() && !already_enabled) {
            const u32 max_bindings = runtime.GetMaxDynamicStorageBuffers();
            if (channel_state->total_graphics_storage_buffers >= max_bindings) {
                LOG_WARNING(HW_GPU,
                            "Skipping graphics storage buffer {} due to driver limit {}",
                            ssbo_index, max_bindings);
                return false;
            }
        }
    }
    channel_state->enabled_storage_buffers[stage] |= 1U << ssbo_index;
    channel_state->written_storage_buffers[stage] |= (is_written ? 1U : 0U) << ssbo_index;
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers() && !already_enabled) {
            ++channel_state->total_graphics_storage_buffers;
        }
    }

    const auto& cbufs = maxwell3d->state.shader_stages[stage];
    GPUVAddr ssbo_addr = cbufs.const_buffers[cbuf_index].address + cbuf_offset;
    if (indirect_offset != ~0U) {
        const GPUVAddr holder = gpu_memory->Read<u64>(ssbo_addr);
        if (holder == 0) {
            channel_state->storage_buffers[stage][ssbo_index] = NULL_BINDING;
            return false;
        }
        ssbo_addr = holder + indirect_offset;
    }
    // A native pointer is bare (no size word after it). Written bindings keep the size word
    // guess: up to 8 MiB marked GPU-written per draw made Ocarina of Time's CPU accesses and MME
    // DMA reads around them flush, at a few frames a second. EDEN_NX2_GRAPHICS_SSBO_SIZE=0: only
    // loaded pointers sized as native, 2: every native binding.
    static const int native_size_mode = [] {
        const char* const value = std::getenv("EDEN_NX2_GRAPHICS_SSBO_SIZE");
        return value != nullptr ? value[0] - '0' : 1;
    }();
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    const bool native_size = indirect_offset != ~0U ||
                             (native && (native_size_mode == 2 ||
                                         (native_size_mode == 1 && !is_written)));
    channel_state->storage_buffers[stage][ssbo_index] = StorageBufferBinding(
        ssbo_addr, cbuf_index, is_written, native_size, indirect_offset != ~0U);
    return (channel_state->storage_buffers[stage][ssbo_index].buffer_id != NULL_BUFFER_ID);
}

template <class P>
void BufferCache<P>::UnbindGraphicsTextureBuffers(size_t stage) {
    if (channel_state->enabled_texture_buffers[stage] == 0) {
        return;
    }
    channel_state->enabled_texture_buffers[stage] = 0;
    channel_state->written_texture_buffers[stage] = 0;
    channel_state->image_texture_buffers[stage] = 0;
}

template <class P>
void BufferCache<P>::BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr,
                                               u32 size, PixelFormat format, bool is_written,
                                               bool is_image) {
    channel_state->enabled_texture_buffers[stage] |= 1U << tbo_index;
    channel_state->written_texture_buffers[stage] |= (is_written ? 1U : 0U) << tbo_index;
    if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
        channel_state->image_texture_buffers[stage] |= (is_image ? 1U : 0U) << tbo_index;
    }
    channel_state->texture_buffers[stage][tbo_index] =
        GetTextureBufferBinding(gpu_addr, size, format);
}

template <class P>
void BufferCache<P>::UnbindComputeStorageBuffers() {
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers()) {
            channel_state->total_compute_storage_buffers -=
                static_cast<u32>(std::popcount(channel_state->enabled_compute_storage_buffers));
        }
    }
    channel_state->enabled_compute_storage_buffers = 0;
    channel_state->written_compute_storage_buffers = 0;
    channel_state->image_compute_texture_buffers = 0;
}

template <class P>
void BufferCache<P>::BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                              bool is_written, u32 indirect_offset) {
    if (ssbo_index >= channel_state->compute_storage_buffers.size()) [[unlikely]] {
        LOG_ERROR(HW_GPU, "Storage buffer index {} exceeds maximum storage buffer count",
                  ssbo_index);
        return;
    }
    const bool already_enabled =
        ((channel_state->enabled_compute_storage_buffers >> ssbo_index) & 1U) != 0;
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers() && !already_enabled) {
            const u32 max_bindings = runtime.GetMaxDynamicStorageBuffers();
            if (channel_state->total_compute_storage_buffers >= max_bindings) {
                LOG_WARNING(HW_GPU,
                            "Skipping compute storage buffer {} due to driver limit {}",
                            ssbo_index, max_bindings);
                return;
            }
        }
    }
    channel_state->enabled_compute_storage_buffers |= u64{1} << ssbo_index;
    channel_state->written_compute_storage_buffers |= u64{is_written ? 1U : 0U} << ssbo_index;
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers() && !already_enabled) {
            ++channel_state->total_compute_storage_buffers;
        }
    }

    const auto& launch_desc = kepler_compute->launch_description;
    if (((launch_desc.const_buffer_enable_mask >> cbuf_index) & 1) == 0) {
        LOG_WARNING(HW_GPU, "Skipped binding SSBO: cbuf index {} is not enabled", cbuf_index);
        return;
    }
    ASSERT(((launch_desc.const_buffer_enable_mask >> cbuf_index) & 1) != 0);

    const auto& cbufs = launch_desc.const_buffer_config;
    GPUVAddr ssbo_addr = cbufs[cbuf_index].Address() + cbuf_offset;
    if (indirect_offset != ~0U) {
        const GPUVAddr holder = gpu_memory->Read<u64>(ssbo_addr);
        if (holder == 0) {
            channel_state->compute_storage_buffers[ssbo_index] = NULL_BINDING;
            return;
        }
        ssbo_addr = holder + indirect_offset;
    }
    channel_state->compute_storage_buffers[ssbo_index] =
        StorageBufferBinding(ssbo_addr, cbuf_index, is_written, kepler_compute->IsDlssDispatch(),
                             indirect_offset != ~0U);
}

template <class P>
void BufferCache<P>::UnbindComputeTextureBuffers() {
    channel_state->enabled_compute_texture_buffers = 0;
    channel_state->written_compute_texture_buffers = 0;
    channel_state->image_compute_texture_buffers = 0;
}

template <class P>
void BufferCache<P>::BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                              PixelFormat format, bool is_written, bool is_image) {
    if (tbo_index >= channel_state->compute_texture_buffers.size()) [[unlikely]] {
        LOG_ERROR(HW_GPU, "Texture buffer index {} exceeds maximum texture buffer count",
                  tbo_index);
        return;
    }
    channel_state->enabled_compute_texture_buffers |= 1U << tbo_index;
    channel_state->written_compute_texture_buffers |= (is_written ? 1U : 0U) << tbo_index;
    if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
        channel_state->image_compute_texture_buffers |= (is_image ? 1U : 0U) << tbo_index;
    }
    channel_state->compute_texture_buffers[tbo_index] =
        GetTextureBufferBinding(gpu_addr, size, format);
}

template <class P>
void BufferCache<P>::FlushCachedWrites() {
    memory_tracker.FlushCachedWrites();
}

template <class P>
bool BufferCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_gpu_modified_ranges.Empty() || !committed_gpu_modified_ranges.empty();
}

template <class P>
void BufferCache<P>::AccumulateFlushes() {
    if (uncommitted_gpu_modified_ranges.Empty()) {
        return;
    }
    committed_gpu_modified_ranges.emplace_back(std::move(uncommitted_gpu_modified_ranges));
}

template <class P>
bool BufferCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    return (!async_buffers.empty() && async_buffers.front().has_value());
}

template <class P>
void BufferCache<P>::CommitAsyncFlushesHigh() {
    AccumulateFlushes();

    if (committed_gpu_modified_ranges.empty()) {
        async_buffers.emplace_back(std::optional<Async_Buffer>{});
        return;
    }

    auto it = committed_gpu_modified_ranges.begin();
    while (it != committed_gpu_modified_ranges.end()) {
        auto& current_intervals = *it;
        auto next_it = std::next(it);
        while (next_it != committed_gpu_modified_ranges.end()) {
            next_it->ForEach([&current_intervals](DAddr start, DAddr end) {
                current_intervals.Subtract(start, end - start);
            });
            next_it++;
        }
        it++;
    }

    boost::container::small_vector<std::pair<BufferCopy, BufferId>, 16> downloads;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    for (const Common::RangeSet<DAddr>& range_set : committed_gpu_modified_ranges) {
        range_set.ForEach([&](DAddr interval_lower, DAddr interval_upper) {
            const std::size_t size = interval_upper - interval_lower;
            const DAddr device_addr = interval_lower;
            ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
                const DAddr buffer_start = buffer.CpuAddr();
                const DAddr buffer_end = buffer_start + buffer.SizeBytes();
                const DAddr new_start = (std::max)(buffer_start, device_addr);
                const DAddr new_end = (std::min)(buffer_end, device_addr + size);
                memory_tracker.ForEachDownloadRange(
                    new_start, new_end - new_start, false,
                    [&](u64 device_addr_out, u64 range_size) {
                        const DAddr buffer_addr = buffer.CpuAddr();
                        const auto add_download = [&](DAddr start, DAddr end) {
                            const u64 new_offset = start - buffer_addr;
                            const u64 new_size = end - start;
                            downloads.push_back({
                                BufferCopy{
                                    .src_offset = new_offset,
                                    .dst_offset = total_size_bytes,
                                    .size = new_size,
                                },
                                buffer_id,
                            });
                            // Align up to avoid cache conflicts
                            constexpr u64 align = 64ULL;
                            constexpr u64 mask = ~(align - 1ULL);
                            total_size_bytes += (new_size + align - 1) & mask;
                            largest_copy = (std::max)(largest_copy, new_size);
                        };

                        gpu_modified_ranges.ForEachInRange(device_addr_out, range_size,
                                                           add_download);
                    });
            });
        });
    }
    committed_gpu_modified_ranges.clear();
    if (downloads.empty()) {
        async_buffers.emplace_back(std::optional<Async_Buffer>{});
        return;
    }
    Nx2Profile::Add(Nx2Profile::Counter::DownloadBytes, total_size_bytes);
    Nx2Profile::Add(Nx2Profile::Counter::DownloadCopies, downloads.size());
    auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes, true);
    boost::container::small_vector<BufferCopy, 4> normalized_copies;
    Common::RangeSet<DAddr> download_ranges;
    runtime.PreCopyBarrier();
    for (auto& [copy, buffer_id] : downloads) {
        copy.dst_offset += download_staging.offset;
        const std::array copies{copy};
        BufferCopy second_copy{copy};
        Buffer& buffer = slot_buffers[buffer_id];
        second_copy.src_offset = static_cast<size_t>(buffer.CpuAddr()) + copy.src_offset;
        const DAddr orig_device_addr = static_cast<DAddr>(second_copy.src_offset);
        if (Nx2Profile::Enabled()) {
            u64 on_dma_pages = 0;
            const auto& pages = Nx2Profile::DmaReadPages();
            for (DAddr page = orig_device_addr >> 12; page <= (orig_device_addr + copy.size - 1) >> 12;
                 ++page) {
                if (pages.contains(page)) {
                    const DAddr begin = (std::max)(orig_device_addr, page << 12);
                    const DAddr end = (std::min)(orig_device_addr + copy.size, (page + 1) << 12);
                    on_dma_pages += end - begin;
                }
            }
            Nx2Profile::Add(Nx2Profile::Counter::DownloadBytesOnDmaPages, on_dma_pages);
        }
        download_ranges.Add(orig_device_addr, copy.size);
        buffer.MarkUsage(copy.src_offset, copy.size);
        runtime.CopyBuffer(download_staging.buffer, buffer, copies, false);
        normalized_copies.push_back(second_copy);
    }
    runtime.PostCopyBarrier();
    pending_downloads.emplace_back(std::move(normalized_copies));
    pending_download_ranges.emplace_back(std::move(download_ranges));
    async_buffers.emplace_back(download_staging);
}

template <class P>
void BufferCache<P>::CommitAsyncFlushes() {
    CommitAsyncFlushesHigh();
}

template <class P>
void BufferCache<P>::PopAsyncFlushes() {
    PopAsyncBuffers();
}

/// EDEN_NX2_KEEP_CPU_WRITES=0: downloads write the GPU's copy over CPU writes not applied yet, and
/// collected writes are applied one by one, as before.
inline bool KeepPendingCpuWrites() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_KEEP_CPU_WRITES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

template <class P>
void BufferCache<P>::ClaimCpuWrites(std::span<const std::pair<DAddr, u64>> ranges) {
    if (!KeepPendingCpuWrites()) {
        return;
    }
    for (const auto& [device_addr, size] : ranges) {
        if (IsRegionGpuModified(device_addr, size)) {
            ClearDownload(device_addr, size);
            gpu_modified_ranges.Subtract(device_addr, size);
        }
    }
}

template <class P>
Common::RangeSet<DAddr> BufferCache<P>::PendingCpuWriteRanges() {
    Common::RangeSet<DAddr> ranges;
    if (pending_cpu_writes && KeepPendingCpuWrites()) {
        pending_cpu_writes(
            [&ranges](DAddr device_addr, size_t size) { ranges.Add(device_addr, size); });
    }
    return ranges;
}

template <class P>
void BufferCache<P>::WriteDownloadedMemory(const Common::RangeSet<DAddr>& cpu_written,
                                           DAddr device_addr, const u8* data, u64 size) {
    if (cpu_written.Empty() || !cpu_written.Intersects(device_addr, size)) {
        device_memory.WriteBlockUnsafe(device_addr, data, size);
        return;
    }
    Common::RangeSet<DAddr> parts;
    parts.Add(device_addr, size);
    cpu_written.ForEachInRange(device_addr, size, [&parts](DAddr start, DAddr end) {
        parts.Subtract(start, end - start);
    });
    parts.ForEach([&](DAddr start, DAddr end) {
        device_memory.WriteBlockUnsafe(start, data + (start - device_addr), end - start);
    });
}

template <class P>
void BufferCache<P>::PopAsyncBuffers() {
    if (async_buffers.empty()) {
        return;
    }
    if (!async_buffers.front().has_value()) {
        async_buffers.pop_front();
        return;
    }
    auto& downloads = pending_downloads.front();
    auto& ranges = pending_download_ranges.front();
    auto& async_buffer = async_buffers.front();
    u8* base = async_buffer->mapped_span.data();
    const size_t base_offset = async_buffer->offset;
    // This runs on the fencing thread while the CPU keeps writing: CPU writes collected since the
    // copy was taken are newer than it, and the GPU thread hasn't dropped them from `ranges` yet.
    const Common::RangeSet<DAddr> cpu_written = PendingCpuWriteRanges();
    for (const auto& copy : downloads) {
        const DAddr device_addr = static_cast<DAddr>(copy.src_offset);
        const u64 dst_offset = copy.dst_offset - base_offset;
        const u8* read_mapped_memory = base + dst_offset;
        // Only what no write replaced since the copy was taken: the rest is older than guest
        // memory (CPU writes) or than the GPU's copy (GPU writes, still to be downloaded).
        ranges.ForEachInRange(device_addr, copy.size, [&](DAddr start, DAddr end) {
            WriteDownloadedMemory(cpu_written, start, &read_mapped_memory[start - device_addr],
                                  end - start);
            gpu_modified_ranges.Subtract(start, end - start);
        });
    }
    async_buffers_death_ring.emplace_back(*async_buffer);
    async_buffers.pop_front();
    pending_downloads.pop_front();
    pending_download_ranges.pop_front();
}

template <class P>
bool BufferCache<P>::IsRegionGpuModified(DAddr addr, size_t size) {
    // One lookup instead of visiting every range inside (~4% of the GPU thread for title 1's
    // inline uploads once GPU-written ranges are no longer cleared by downloads).
    return gpu_modified_ranges.Intersects(addr, size);
}

template <class P>
bool BufferCache<P>::IsRegionRegistered(DAddr addr, size_t size) {
    const DAddr end_addr = addr + size;
    const u64 page_end = Common::DivCeil(end_addr, CACHING_PAGESIZE);
    for (u64 page = addr >> CACHING_PAGEBITS; page < page_end;) {
        const BufferId buffer_id = page_table[page];
        if (!buffer_id) {
            ++page;
            continue;
        }
        Buffer& buffer = slot_buffers[buffer_id];
        const DAddr buf_start_addr = buffer.CpuAddr();
        const DAddr buf_end_addr = buf_start_addr + buffer.SizeBytes();
        if (buf_start_addr < end_addr && addr < buf_end_addr) {
            return true;
        }
        page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
    }
    return false;
}

template <class P>
bool BufferCache<P>::IsRegionCpuModified(DAddr addr, size_t size) {
    return memory_tracker.IsRegionCpuModified(addr, size);
}

template <class P>
void BufferCache<P>::BindHostIndexBuffer() {
    Nx2Profile::Scope profile_scope{Nx2Profile::Zone::BindIndex};
    Buffer& buffer = slot_buffers[channel_state->index_buffer.buffer_id];
    TouchBuffer(buffer, channel_state->index_buffer.buffer_id);
    const u32 offset = buffer.Offset(channel_state->index_buffer.device_addr);
    const u32 size = channel_state->index_buffer.size;
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (draw_state.inline_index_draw_indexes.empty()) {
        SynchronizeBuffer(buffer, channel_state->index_buffer.device_addr, size);
    } else {
        if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
            auto upload_staging = runtime.UploadStagingBuffer(size);
            std::array<BufferCopy, 1> copies{{BufferCopy{.src_offset = upload_staging.offset, .dst_offset = 0, .size = size}}};
            std::memcpy(upload_staging.mapped_span.data(), draw_state.inline_index_draw_indexes.data(), size);
            // The inline index buffer is no guest memory: no pages.
            NoteGpuWrite(buffer, 0, 0);
            runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true);
        } else {
            buffer.ImmediateUpload(0, draw_state.inline_index_draw_indexes);
        }
    }
    if constexpr (HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT) {
        const u32 new_offset = offset + draw_state.index_buffer.first * u32(draw_state.index_buffer.FormatSizeInBytes());
        runtime.BindIndexBuffer(buffer, new_offset, size);
    } else {
        buffer.MarkUsage(offset, size);
        runtime.BindIndexBuffer(draw_state.topology, draw_state.index_buffer.format, draw_state.index_buffer.first, draw_state.index_buffer.count, buffer, offset, size);
    }
}

template <class P>
void BufferCache<P>::BindHostVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size,
                                          u32 stride) {
    if constexpr (IS_OPENGL) {
        runtime.BindVertexBuffer(index, buffer, offset, size, stride);
    } else {
        runtime.BindVertexBuffer(index, buffer.Handle(), offset, size, stride);
    }
}

template <class P>
Binding& BufferCache<P>::VertexBufferSlot(u32 index) {
    ASSERT(index < NUM_VERTEX_BUFFERS);
    return v_buffer[index];
}

template <class P>
const Binding& BufferCache<P>::VertexBufferSlot(u32 index) const {
    ASSERT(index < NUM_VERTEX_BUFFERS);
    return v_buffer[index];
}

template <class P>
void BufferCache<P>::UpdateVertexBufferSlot(u32 index, const Binding& binding) {
    Binding& slot = VertexBufferSlot(index);
    if (slot.device_addr != binding.device_addr || slot.size != binding.size) {
        ++vertex_buffers_serial;
    }
    slot = binding;
    if (binding.buffer_id != NULL_BUFFER_ID && binding.size != 0) {
        enabled_vertex_buffers_mask |= (1u << index);
    } else {
        enabled_vertex_buffers_mask &= ~(1u << index);
    }
}

template <class P>
void BufferCache<P>::MarkVertexBuffersStale(DAddr device_addr, u64 size) {
    if constexpr (requires { runtime.IsVertexRepacked(u32{}); }) {
        u32 mask = enabled_vertex_buffers_mask;
        while (mask != 0) {
            const u32 index = std::countr_zero(mask);
            mask &= mask - 1;
            const Binding& binding = v_buffer[index];
            if (device_addr < binding.device_addr + binding.size &&
                binding.device_addr < device_addr + size) {
                stale_vertex_buffers_mask |= 1U << index;
            }
        }
    }
}

template <class P>
void BufferCache<P>::RefreshRepackedVertexBuffers() {
    if constexpr (requires { runtime.IsVertexRepacked(u32{}); }) {
        u32 mask = std::exchange(stale_vertex_buffers_mask, 0) & enabled_vertex_buffers_mask;
        auto& flags = maxwell3d->dirty.flags;
        while (mask != 0) {
            const u32 index = std::countr_zero(mask);
            mask &= mask - 1;
            if (runtime.IsVertexRepacked(index)) {
                flags[Dirty::VertexBuffer0 + index] = true;
            }
        }
    }
}

template <class P>
void BufferCache<P>::BindHostVertexBuffers() {
    Nx2Profile::Scope profile_scope{Nx2Profile::Zone::BindVertex};
    RefreshRepackedVertexBuffers();

#ifdef __ANDROID__
    const bool use_optimized_vertex_buffers = Settings::values.use_optimized_vertex_buffers.GetValue();
#else
    constexpr bool use_optimized_vertex_buffers = true;
#endif

    if (use_optimized_vertex_buffers) {
        auto& flags = maxwell3d->dirty.flags;
        u32 enabled_mask = enabled_vertex_buffers_mask;
        if (nx2_used_vertex_buffers != ~0U) {
            if (nx2_skip_unused_vertex_buffers) {
                enabled_mask &= nx2_used_vertex_buffers;
            } else {
                Nx2NoteUnusedVertexBuffers(enabled_mask & ~nx2_used_vertex_buffers);
            }
        }
        HostBindings<Buffer> bindings{};
        u32 last_index = (std::numeric_limits<u32>::max)();
        const auto flush_bindings = [&]() {
            if (bindings.buffers.empty()) {
                return;
            }
            bindings.max_index = bindings.min_index + static_cast<u32>(bindings.buffers.size());
            runtime.BindVertexBuffers(bindings);
            bindings = HostBindings<Buffer>{};
            last_index = (std::numeric_limits<u32>::max)();
        };
        while (enabled_mask != 0) {
            const u32 index = std::countr_zero(enabled_mask);
            enabled_mask &= (enabled_mask - 1);
            const Binding& binding = VertexBufferSlot(index);
            Buffer& buffer = slot_buffers[binding.buffer_id];
            TouchBuffer(buffer, binding.buffer_id);
            SynchronizeBuffer(buffer, binding.device_addr, binding.size);
            if (stale_vertex_buffers_mask != 0) [[unlikely]] {
                // A hot page upload (EDEN_NX2_HOT_PAGES) into a repacked vertex buffer.
                RefreshRepackedVertexBuffers();
            }
            if (!flags[Dirty::VertexBuffer0 + index]) {
                flush_bindings();
                continue;
            }
            flags[Dirty::VertexBuffer0 + index] = false;
            const u32 stride = maxwell3d->regs.vertex_streams[index].stride;
            const u32 offset = buffer.Offset(binding.device_addr);
            buffer.MarkUsage(offset, binding.size);
            if (!bindings.buffers.empty() && index != last_index + 1) {
                flush_bindings();
            }
            if (bindings.buffers.empty()) {
                bindings.min_index = index;
            }
            bindings.buffers.push_back(&buffer);
            bindings.offsets.push_back(offset);
            bindings.sizes.push_back(binding.size);
            bindings.strides.push_back(stride);
            last_index = index;
        }
        flush_bindings();
    } else {
        HostBindings<typename P::Buffer> host_bindings;
        bool any_valid{false};
        auto& flags = maxwell3d->dirty.flags;
        // EDEN_NX2_FAST_BUFFERS: streams the vertex shader doesn't read keep their dirty flags.
        u32 used = ~0U;
        if (nx2_used_vertex_buffers != ~0U) {
            if (nx2_skip_unused_vertex_buffers) {
                used = nx2_used_vertex_buffers;
            } else {
                Nx2NoteUnusedVertexBuffers(~nx2_used_vertex_buffers);
            }
        }
        for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
            if (((used >> index) & 1) == 0) {
                continue;
            }
            const Binding& binding = channel_state->vertex_buffers[index];
            Buffer& buffer = slot_buffers[binding.buffer_id];
            TouchBuffer(buffer, binding.buffer_id);
            SynchronizeBuffer(buffer, binding.device_addr, binding.size);
            if (stale_vertex_buffers_mask != 0) [[unlikely]] {
                RefreshRepackedVertexBuffers();
            }
            if (!flags[Dirty::VertexBuffer0 + index]) {
                continue;
            }
            flags[Dirty::VertexBuffer0 + index] = false;

            host_bindings.min_index = (std::min)(host_bindings.min_index, index);
            host_bindings.max_index = (std::max)(host_bindings.max_index, index);
            any_valid = true;
        }

        if (any_valid) {
            host_bindings.max_index++;
            for (u32 index = host_bindings.min_index; index < host_bindings.max_index; index++) {
                flags[Dirty::VertexBuffer0 + index] = false;

                const Binding& binding = channel_state->vertex_buffers[index];
                Buffer& buffer = slot_buffers[binding.buffer_id];

                const u32 stride = maxwell3d->regs.vertex_streams[index].stride;
                const u32 offset = buffer.Offset(binding.device_addr);
                buffer.MarkUsage(offset, binding.size);

                host_bindings.buffers.push_back(&buffer);
                host_bindings.offsets.push_back(offset);
                host_bindings.sizes.push_back(binding.size);
                host_bindings.strides.push_back(stride);
            }
            runtime.BindVertexBuffers(host_bindings);
        }
    }
}

template <class P>
void BufferCache<P>::Nx2NoteUnusedVertexBuffers(u32 unused) {
    auto& stats = Tegra::g_nx2_quick_bind;
    stats.vertex_draws.fetch_add(1, std::memory_order_relaxed);
    u32 pending = 0;
    u32 count = 0;
    for (u32 mask = unused; mask != 0; mask &= mask - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(mask));
        if (index >= NUM_VERTEX_BUFFERS) {
            break;
        }
        ++count;
        const Binding& binding = channel_state->vertex_buffers[index];
        if (binding.size != 0 &&
            memory_tracker.HasPendingCpuUpload(binding.device_addr, binding.size)) {
            ++pending;
        }
    }
    stats.vertex_skipped.fetch_add(count, std::memory_order_relaxed);
    stats.vertex_pending.fetch_add(pending, std::memory_order_relaxed);
    Tegra::Nx2ReportQuickBind();
}

template <class P>
void BufferCache<P>::BindHostDrawIndirectBuffers() {
    const auto bind_buffer = [this](const Binding& binding) {
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        SynchronizeBuffer(buffer, binding.device_addr, binding.size);
    };
    if (current_draw_indirect->include_count) {
        bind_buffer(channel_state->count_buffer_binding);
    }
    bind_buffer(channel_state->indirect_buffer_binding);
}

template <class P>
void BufferCache<P>::BindHostGraphicsUniformBuffers(size_t stage) {
    u32 dirty = ~0U;
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        dirty = std::exchange(channel_state->dirty_uniform_buffers[stage], 0);
    }
    u32 binding_index = 0;
    ForEachEnabledBit(channel_state->enabled_uniform_buffer_masks[stage], [&](u32 index) {
        const bool needs_bind = ((dirty >> index) & 1) != 0;
        BindHostGraphicsUniformBuffer(stage, index, binding_index, needs_bind);
        if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
            ++binding_index;
        }
    });
}

template <class P>
void BufferCache<P>::BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index, bool needs_bind) {
    ++channel_state->uniform_cache_shots[0];
    // Bank 24 (root table 0, NVN2's push constants) is read from bank 17, which such games do not
    // bind: stream the table's current words as that buffer.
    if (index == 17 && maxwell3d->root_tables.loaded && channel_state->uniform_buffers[stage][index].size == 0) {
        const auto& words = maxwell3d->root_tables.tables[0];
        const u32 size = (*channel_state->uniform_buffer_sizes)[stage][index];
        const u32 copied = (std::min)(size, static_cast<u32>(sizeof(words)));
        if constexpr (IS_OPENGL) {
            std::array<u8, sizeof(words)> data{};
            std::memcpy(data.data(), words.data(), copied);
            runtime.BindFastUniformBuffer(stage, binding_index, copied);
            runtime.PushFastUniformBuffer(stage, binding_index, std::span<const u8>(data.data(), copied));
        } else {
            const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            std::memset(span.data(), 0, span.size());
            std::memcpy(span.data(), words.data(), copied);
        }
        channel_state->fast_bound_uniform_buffers[stage] |= 1u << binding_index;
        channel_state->uniform_buffer_binding_sizes[stage][binding_index] = size;
        return;
    }
    const Binding& binding = channel_state->uniform_buffers[stage][index];
    const DAddr device_addr = binding.device_addr;
    const u32 size = (std::min)(binding.size, (*channel_state->uniform_buffer_sizes)[stage][index]);
    if (Nx2Profile::Enabled()) {
        auto& previous = nx2_previous_ubos[stage][index];
        Nx2Profile::Add(Nx2Profile::Counter::UboBinds, 1);
        Nx2Profile::Add(Nx2Profile::Counter::UboSame,
                        previous.first == device_addr && previous.second == size ? 1 : 0);
        previous = {device_addr, size};
    }
    Buffer& buffer = slot_buffers[binding.buffer_id];
    TouchBuffer(buffer, binding.buffer_id);
    const bool has_host_buffer = binding.buffer_id != NULL_BUFFER_ID;
    const u32 offset = has_host_buffer ? buffer.Offset(device_addr) : 0;
    const bool needs_alignment_stream = [&]() {
        if constexpr (IS_OPENGL) {
            return false;
        } else {
            if (!has_host_buffer) {
                return false;
            }
            const u32 alignment = runtime.GetUniformBufferAlignment();
            return alignment > 1 && (offset % alignment) != 0;
        }
    }();
    // Native programs keep tables next to their code in uniform banks read with indirect offsets
    // (Splatoon Raiders: the dialogue box corners in c1). Writes to that memory can go unnoticed by
    // the memory tracker, leaving stale host contents, so upload such a bank again.
    // EDEN_NX2_BANK_REUPLOAD: 0 off, 1 a 64 KiB bank once per frame (the default), 2 a 64 KiB
    // bank on every bind (once per frame left Ocarina of Time's per-draw material data stale for a
    // frame now and then: white flashes; costs back-thread time), 3 every native bank on every bind.
    static const char bank_mode = [] {
        const char* const value = std::getenv("EDEN_NX2_BANK_REUPLOAD");
        return value != nullptr && value[0] != '\0' ? value[0] : '1';
    }();
    {
        Nx2Profile::Scope bank_scope{Nx2Profile::Zone::UboBank};
        if (bank_mode != '0' && has_host_buffer && size != 0 && gpu_memory != nullptr &&
            gpu_memory->UsesNativeTiling() &&
            (bank_mode == '3' || (*channel_state->uniform_buffer_sizes)[stage][index] == 0x10000) &&
            !memory_tracker.IsRegionGpuModified(device_addr, size) &&
            (bank_mode == '2' || bank_mode == '3' ||
             native_indirect_uniforms_this_frame.insert(device_addr).second)) {
            // The bank size is only an upper limit: stop at the end of the mapped memory.
            u64 mapped_size = 0;
            while (mapped_size < size &&
                   device_memory.template GetPointer<u8>(device_addr + mapped_size) != nullptr) {
                mapped_size += Core::DEVICE_PAGESIZE -
                               ((device_addr + mapped_size) & Core::DEVICE_PAGEMASK);
            }
            if (mapped_size != 0) {
                const u64 bank_size = (std::min<u64>)(mapped_size, size);
                if (HotPages::Enabled() && HotPages::KeepBankProtection()) {
                    // EDEN_NX2_HOT_PAGES: upload the bank again but keep it write-protected, so
                    // its upload doesn't protect it again (an unprotect and a protect per page a
                    // frame on the back thread). A CPU write to it still faults as before.
                    memory_tracker.MarkRegionForUpload(device_addr, bank_size);
                    hot_bank_pages += Common::DivCeil(bank_size, Core::DEVICE_PAGESIZE);
                } else {
                    memory_tracker.MarkRegionAsCpuModified(device_addr, bank_size);
                }
            }
        }
    }
    const bool small_host = has_host_buffer && size <= channel_state->uniform_buffer_skip_cache_size;
    const bool gpu_modified = [&] {
        Nx2Profile::Scope scope{Nx2Profile::Zone::UboTracker};
        return small_host && memory_tracker.IsRegionGpuModified(device_addr, size);
    }();
    const bool use_fast_buffer = needs_alignment_stream || (small_host && !gpu_modified);
    if (use_fast_buffer) {
        if constexpr (IS_OPENGL) {
            if (runtime.HasFastBufferSubData()) {
                // Fast path for Nvidia
                const bool should_fast_bind =
                    !HasFastUniformBufferBound(stage, binding_index) ||
                    channel_state->uniform_buffer_binding_sizes[stage][binding_index] != size;
                if (should_fast_bind) {
                    // We only have to bind when the currently bound buffer is not the fast version
                    channel_state->fast_bound_uniform_buffers[stage] |= 1u << binding_index;
                    channel_state->uniform_buffer_binding_sizes[stage][binding_index] = size;
                    runtime.BindFastUniformBuffer(stage, binding_index, size);
                }
                const auto span = ImmediateBufferWithData(device_addr, size);
                runtime.PushFastUniformBuffer(stage, binding_index, span);
                return;
            }
        }
        channel_state->fast_bound_uniform_buffers[stage] |= 1u << binding_index;
        channel_state->uniform_buffer_binding_sizes[stage][binding_index] = size;
        // Stream buffer path to avoid stalling on non-Nvidia drivers or Vulkan
        bool streamed = false;
        if constexpr (!IS_OPENGL && VideoCommon::StreamUniformReusable<Runtime>) {
            // The same bytes as this slot's previous bind, in the same command buffer: bind that
            // stream allocation again instead of allocating and copying (uniform_stream_reuse.h).
            // EDEN_NX2_UBOREUSE=0 turns it off.
            if (VideoCommon::UniformStreamReuseEnabled()) {
                Nx2Profile::SampledScope<3> scope{Nx2Profile::Zone::UboReuse};
                streamed = VideoCommon::BindSmallUniformReusing(
                    runtime, device_memory, stage, index, binding_index, device_addr, size);
                if (streamed) {
                    Nx2Profile::Add(Nx2Profile::Counter::UboReuseHits, 1);
                }
            }
        }
        if (!streamed) {
            Nx2Profile::SampledScope<3> scope{Nx2Profile::Zone::UboCopy};
            Nx2Profile::Add(Nx2Profile::Counter::UboCopyBytes, size);
            const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            device_memory.ReadBlockUnsafe(device_addr, span.data(), size);
        }
        return;
    }
    // Classic cached path
    Nx2Profile::SampledScope<3> cached_scope{Nx2Profile::Zone::UboCached};
    if (SynchronizeBuffer(buffer, device_addr, size)) {
        ++channel_state->uniform_cache_hits[0];
    }
    // Skip binding if it's not needed and if the bound buffer is not the fast version
    // This exists to avoid instances where the fast buffer is bound and a GPU write happens
    needs_bind |= HasFastUniformBufferBound(stage, binding_index);
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        needs_bind |= channel_state->uniform_buffer_binding_sizes[stage][binding_index] != size;
    }
    if (!needs_bind) {
        return;
    }
    if constexpr (IS_OPENGL) {
        // Mark the index as dirty if offset doesn't match
        const bool is_copy_bind = offset != 0 && !runtime.SupportsNonZeroUniformOffset();
        channel_state->dirty_uniform_buffers[stage] |= (is_copy_bind ? 1U : 0U) << index;
    }
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        channel_state->uniform_buffer_binding_sizes[stage][binding_index] = size;
    }
    buffer.MarkUsage(offset, size);
    if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
        runtime.BindUniformBuffer(stage, binding_index, buffer, offset, size);
    } else {
        runtime.BindUniformBuffer(buffer, offset, size);
    }
    channel_state->fast_bound_uniform_buffers[stage] &= ~(1u << binding_index);
}

template <class P>
void BufferCache<P>::ResolveMultiRangeStorage(Binding& binding, bool is_written,
                                              std::vector<MultiRangeSegment>& pool) {
    binding.segment_first = 0;
    binding.segment_count = 0;
    if constexpr (requires { runtime.BindMultiRangeStorageBuffer(u64{}, bool{}); }) {
        if (binding.gpu_addr == 0 || binding.size == 0) {
            return;
        }
        if (is_written && !runtime.PrefersSparseSources()) {
            return;
        }
        const VirtualSegments* found =
            virtual_ranges.Query(*gpu_memory, binding.gpu_addr, binding.size);
        if (!found || found->size() < 2) {
            return;
        }
        const VirtualSegments segments = *found;
        const u32 first = static_cast<u32>(pool.size());
        const bool prefer_sparse = runtime.PrefersSparseSources();
        for (const VirtualSegment& segment : segments) {
            const BufferId buffer_id =
                FindBuffer(segment.device_addr, segment.size, prefer_sparse);
            if (!buffer_id) {
                pool.resize(first);
                return;
            }
            pool.push_back(MultiRangeSegment{
                .buffer_id = buffer_id,
                .device_addr = segment.device_addr,
                .size = segment.size,
            });
        }
        binding.segment_first = first;
        binding.segment_count = static_cast<u32>(segments.size());
    }
}

template <class P>
bool BufferCache<P>::BindMultiRangeStorage(const Binding& binding, bool is_written,
                                           std::span<const MultiRangeSegment> pool) {
    if constexpr (requires { runtime.BindMultiRangeStorageBuffer(u64{}, bool{}); }) {
        if (binding.segment_count < 2) {
            return false;
        }
        if (binding.segment_first + binding.segment_count > pool.size()) {
            return false;
        }
        // A segment whose buffer was joined into another (deleted) since it was resolved: binding
        // the dead slot put its freed LRU item back in the list, and the collector later deleted
        // the slot again, wiping another buffer's page entries.
        for (u32 index = 0; index < binding.segment_count; ++index) {
            const BufferId id = pool[binding.segment_first + index].buffer_id;
            if (!id || !slot_buffers.contains(id)) [[unlikely]] {
                return false;
            }
        }
        const u64 key = (static_cast<u64>(gpu_memory->GetID()) << 48) ^ binding.gpu_addr;
        runtime.ResetMultiRange();
        for (u32 index = 0; index < binding.segment_count; ++index) {
            const MultiRangeSegment& segment = pool[binding.segment_first + index];
            Buffer& buffer = slot_buffers[segment.buffer_id];
            TouchBuffer(buffer, segment.buffer_id);
            if (SynchronizeBuffer(buffer, segment.device_addr, segment.size)) {
                runtime.InvalidateMultiRange(key);
            }
            const u32 offset = buffer.Offset(segment.device_addr);
            buffer.MarkUsage(offset, segment.size);
            if (is_written) {
                MarkWrittenBuffer(segment.buffer_id, segment.device_addr, segment.size);
            }
            runtime.PushMultiRangeSource(buffer, offset, segment.size);
        }
        return runtime.BindMultiRangeStorageBuffer(key, is_written);
    } else {
        return false;
    }
}

template <class P>
void BufferCache<P>::BindHostGraphicsStorageBuffers(size_t stage) {
    u32 binding_index = 0;
    ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
        const Binding& binding = channel_state->storage_buffers[stage][index];
        const bool is_written = ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0;
        if (Nx2Profile::Enabled()) {
            auto& previous = nx2_previous_ssbos[stage][index];
            Nx2Profile::Add(Nx2Profile::Counter::SsboBinds, 1);
            Nx2Profile::Add(Nx2Profile::Counter::SsboSame,
                            previous.first == binding.device_addr &&
                                    previous.second == binding.size
                                ? 1
                                : 0);
            Nx2Profile::Add(Nx2Profile::Counter::SsboWritten, is_written ? 1 : 0);
            previous = {binding.device_addr, binding.size};
        }
        if (BindMultiRangeStorage(binding, is_written, graphics_segments)) {
            return;
        }
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.device_addr, size);

        const u32 offset = buffer.Offset(binding.device_addr);
        buffer.MarkUsage(offset, size);

        if (is_written) {
            MarkWrittenBuffer(binding.buffer_id, binding.device_addr, size);
        }

        if constexpr (NEEDS_BIND_STORAGE_INDEX) {
            runtime.BindStorageBuffer(stage, binding_index, buffer, offset, size, is_written);
            ++binding_index;
        } else {
            runtime.BindStorageBuffer(buffer, offset, size, is_written);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostGraphicsTextureBuffers(size_t stage) {
    ForEachEnabledBit(channel_state->enabled_texture_buffers[stage], [&](u32 index) {
        const TextureBufferBinding& binding = channel_state->texture_buffers[stage][index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.device_addr, size);

        const bool is_written = ((channel_state->written_texture_buffers[stage] >> index) & 1) != 0;
        if (is_written) {
            MarkWrittenBuffer(binding.buffer_id, binding.device_addr, size);
        }

        const u32 offset = buffer.Offset(binding.device_addr);
        const PixelFormat format = binding.format;
        buffer.MarkUsage(offset, size);
        if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
            if (((channel_state->image_texture_buffers[stage] >> index) & 1) != 0) {
                runtime.BindImageBuffer(buffer, offset, size, format);
            } else {
                runtime.BindTextureBuffer(buffer, offset, size, format);
            }
        } else {
            runtime.BindTextureBuffer(buffer, offset, size, format);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostTransformFeedbackBuffers() {
    if (maxwell3d->regs.transform_feedback_enabled == 0) {
        return;
    }
    HostBindings<typename P::Buffer> host_bindings;
    for (u32 index = 0; index < NUM_TRANSFORM_FEEDBACK_BUFFERS; ++index) {
        const Binding& binding = channel_state->transform_feedback_buffers[index];
        const auto& control = maxwell3d->regs.transform_feedback.controls[index];
        const bool has_layout = control.varying_count != 0 || control.stride != 0;

        Buffer* host_buffer = &slot_buffers[NULL_BUFFER_ID];
        u32 offset = 0;
        u32 size = 0;

        if (has_layout && binding.buffer_id != NULL_BUFFER_ID && binding.size != 0) {
            Buffer& buffer = slot_buffers[binding.buffer_id];
            TouchBuffer(buffer, binding.buffer_id);
            size = binding.size;
            SynchronizeBuffer(buffer, binding.device_addr, size);
            MarkWrittenBuffer(binding.buffer_id, binding.device_addr, size);
            offset = buffer.Offset(binding.device_addr);
            buffer.MarkUsage(offset, size);
            host_buffer = &buffer;
        }

        host_bindings.buffers.push_back(host_buffer);
        host_bindings.offsets.push_back(offset);
        host_bindings.sizes.push_back(size);
    }
    runtime.BindTransformFeedbackBuffers(host_bindings);
}

template <class P>
void BufferCache<P>::BindHostComputeUniformBuffers() {
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        // Mark all uniform buffers as dirty
        channel_state->dirty_uniform_buffers.fill(~u32{0});
        channel_state->fast_bound_uniform_buffers.fill(0);
    }
    u32 binding_index = 0;
    ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
        const Binding& binding = channel_state->compute_uniform_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size =
            (std::min)(binding.size, (*channel_state->compute_uniform_buffer_sizes)[index]);
        const bool has_host_buffer = binding.buffer_id != NULL_BUFFER_ID;
        const u32 offset = has_host_buffer ? buffer.Offset(binding.device_addr) : 0;
        const bool needs_alignment_stream = [&]() {
            if constexpr (IS_OPENGL) {
                return false;
            } else {
                if (!has_host_buffer) {
                    return false;
                }
                const u32 alignment = runtime.GetUniformBufferAlignment();
                return alignment > 1 && (offset % alignment) != 0;
            }
        }();
        // Small buffers the GPU has not written are streamed, as graphics ones are: synchronizing
        // them made every dispatch re-protect pages an inline upload had just unprotected (two
        // VirtualProtect calls per dispatch, ~4,000 dispatches a second in title 1).
        const bool use_fast_buffer =
            needs_alignment_stream ||
            (has_host_buffer && size <= channel_state->uniform_buffer_skip_cache_size &&
             !memory_tracker.IsRegionGpuModified(binding.device_addr, size));
        if constexpr (!IS_OPENGL) {
            if (use_fast_buffer) {
                const std::span<u8> span =
                    runtime.BindMappedUniformBuffer(0, binding_index, size);
                device_memory.ReadBlockUnsafe(binding.device_addr, span.data(), size);
                return;
            }
        }

        SynchronizeBuffer(buffer, binding.device_addr, size);

        buffer.MarkUsage(offset, size);
        if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
            runtime.BindComputeUniformBuffer(binding_index, buffer, offset, size);
            ++binding_index;
        } else {
            runtime.BindUniformBuffer(buffer, offset, size);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostComputeStorageBuffers() {
    u32 binding_index = 0;
    ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
        const Binding& binding = channel_state->compute_storage_buffers[index];
        const bool is_written =
            ((channel_state->written_compute_storage_buffers >> index) & 1) != 0;
        if (BindMultiRangeStorage(binding, is_written, compute_segments)) {
            return;
        }
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.device_addr, size);

        const u32 offset = buffer.Offset(binding.device_addr);
        buffer.MarkUsage(offset, size);

        if (is_written) {
            MarkWrittenBuffer(binding.buffer_id, binding.device_addr, size, true);
        }

        if constexpr (NEEDS_BIND_STORAGE_INDEX) {
            runtime.BindComputeStorageBuffer(binding_index, buffer, offset, size, is_written);
            ++binding_index;
        } else {
            runtime.BindStorageBuffer(buffer, offset, size, is_written);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostComputeTextureBuffers() {
    ForEachEnabledBit(channel_state->enabled_compute_texture_buffers, [&](u32 index) {
        const TextureBufferBinding& binding = channel_state->compute_texture_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.device_addr, size);

        const bool is_written =
            ((channel_state->written_compute_texture_buffers >> index) & 1) != 0;
        if (is_written) {
            MarkWrittenBuffer(binding.buffer_id, binding.device_addr, size);
        }

        const u32 offset = buffer.Offset(binding.device_addr);
        const PixelFormat format = binding.format;
        buffer.MarkUsage(offset, size);
        if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
            if (((channel_state->image_compute_texture_buffers >> index) & 1) != 0) {
                runtime.BindImageBuffer(buffer, offset, size, format);
            } else {
                runtime.BindTextureBuffer(buffer, offset, size, format);
            }
        } else {
            runtime.BindTextureBuffer(buffer, offset, size, format);
        }
    });
}

template <class P>
void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {
    graphics_segments.clear();
    BufferOperations([&]() {
        if (is_indexed) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::UpdateIndex};
            UpdateIndexBuffer();
        }
        {
            Nx2Profile::Scope scope{Nx2Profile::Zone::UpdateVertex};
            UpdateVertexBuffers();
        }
        {
            Nx2Profile::Scope scope{Nx2Profile::Zone::UpdateOther};
            UpdateTransformFeedbackBuffers();
        }
        {
            Nx2Profile::Scope scope{Nx2Profile::Zone::UpdateStages};
            for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
                UpdateUniformBuffers(stage);
                UpdateStorageBuffers(stage);
                UpdateTextureBuffers(stage);
            }
        }
        Nx2Profile::Scope scope{Nx2Profile::Zone::UpdateOther};
        if (current_draw_indirect) {
            UpdateDrawIndirect();
        }
        RefreshMultiRangeSegments(graphics_segments);
    });
}

template <class P>
void BufferCache<P>::DoUpdateComputeBuffers() {
    compute_segments.clear();
    BufferOperations([&]() {
        UpdateComputeUniformBuffers();
        UpdateComputeStorageBuffers();
        UpdateComputeTextureBuffers();
        RefreshMultiRangeSegments(compute_segments);
    });
}

template <class P>
void BufferCache<P>::RefreshMultiRangeSegments(std::vector<MultiRangeSegment>& pool) {
    if constexpr (requires { runtime.BindMultiRangeStorageBuffer(u64{}, bool{}); }) {
        // Resolving one segment can join, and so delete, the buffer of another (DeleteBuffer
        // clears its id): repeat until every segment points at a live buffer.
        for (int pass = 0; pass < 16; ++pass) {
            bool resolved = false;
            for (MultiRangeSegment& segment : pool) {
                if (!segment.buffer_id) {
                    segment.buffer_id = FindBuffer(segment.device_addr, segment.size,
                                                   runtime.PrefersSparseSources());
                    resolved = true;
                }
            }
            if (!resolved) {
                return;
            }
        }
    }
}

template <class P>
void BufferCache<P>::UpdateIndexBuffer() {
    // We have to check for the dirty flags and index count
    // The index count is currently changed without updating the dirty flags
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const auto& index_buffer_ref = draw_state.index_buffer;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::IndexBuffer]) {
        return;
    }
    flags[Dirty::IndexBuffer] = false;
    if (!draw_state.inline_index_draw_indexes.empty()) [[unlikely]] {
        auto inline_index_size = static_cast<u32>(draw_state.inline_index_draw_indexes.size());
        u32 buffer_size = Common::AlignUp(inline_index_size, CACHING_PAGESIZE);
        if (inline_buffer_id == NULL_BUFFER_ID) [[unlikely]] {
            LOG_INFO(HW_GPU, "Creating inline index buffer of {:#x} bytes", buffer_size);
            inline_buffer_id = CreateBuffer(0, buffer_size, false);
        }
        if (slot_buffers[inline_buffer_id].SizeBytes() < buffer_size) [[unlikely]] {
            LOG_INFO(HW_GPU, "Growing inline index buffer to {:#x} bytes", buffer_size);
            // Erasing the slot directly left its page table entries and LRU item behind, so a
            // later CreateBuffer could reuse the slot and join the new buffer with itself.
            DeleteBuffer(inline_buffer_id, true);
            inline_buffer_id = CreateBuffer(0, buffer_size, false);
        }
        channel_state->index_buffer = Binding{
            .device_addr = 0,
            .size = inline_index_size,
            .buffer_id = inline_buffer_id,
        };
        return;
    }

    // A format from garbage commands (FormatSizeInBytes would abort): no index buffer.
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;
    if (index_buffer_ref.format != IndexFormat::UnsignedByte &&
        index_buffer_ref.format != IndexFormat::UnsignedShort &&
        index_buffer_ref.format != IndexFormat::UnsignedInt) [[unlikely]] {
        channel_state->index_buffer = NULL_BINDING;
        return;
    }
    const GPUVAddr gpu_addr_begin = index_buffer_ref.StartAddress();
    const GPUVAddr gpu_addr_end = index_buffer_ref.EndAddress();
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    const u32 draw_size = (index_buffer_ref.count + index_buffer_ref.first) * u32(index_buffer_ref.FormatSizeInBytes());
    const u32 size = (std::min)(address_size, draw_size);
    if (size == 0 || !device_addr) {
        channel_state->index_buffer = NULL_BINDING;
        return;
    }
    const Binding index_binding{
        .device_addr = *device_addr,
        .size = size,
        .buffer_id = FindBuffer(*device_addr, size, false),
    };
    if (Nx2Profile::Enabled()) {
        const Binding& previous = channel_state->index_buffer;
        Nx2Profile::Add(Nx2Profile::Counter::IndexUpdates, 1);
        Nx2Profile::Add(Nx2Profile::Counter::IndexSame,
                        previous.device_addr == index_binding.device_addr &&
                                previous.size == index_binding.size &&
                                previous.buffer_id == index_binding.buffer_id
                            ? 1
                            : 0);
    }
    channel_state->index_buffer = index_binding;
}

template <class P>
void BufferCache<P>::UpdateVertexBuffers() {
    auto& flags = maxwell3d->dirty.flags;
    if (!maxwell3d->dirty.flags[Dirty::VertexBuffers]) {
        return;
    }
    flags[Dirty::VertexBuffers] = false;

    for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
        UpdateVertexBuffer(index);
    }
}

template <class P>
void BufferCache<P>::UpdateVertexBuffer(u32 index) {
    if (!maxwell3d->dirty.flags[Dirty::VertexBuffer0 + index]) {
        return;
    }
    const auto& array = maxwell3d->regs.vertex_streams[index];
    const auto& limit = maxwell3d->regs.vertex_stream_limits[index];
    const GPUVAddr gpu_addr_begin = array.Address();
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    u64 native_size = native ? maxwell3d->regs.NativeVertexStreamSize(index) : 0;
    if (native && array.enable != 0 && native_size > (std::numeric_limits<u32>::max)()) {
        // A misread or stale size: bounded by what's mapped below instead. Thrown on the GPU
        // thread, it aborted Mario Kart World as it booted.
        static std::atomic<u32> reports{};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_WARNING(HW_GPU, "Native vertex stream {} size {:#x} exceeds 32 bits, clamped", index,
                        native_size);
        }
        native_size = (std::numeric_limits<u32>::max)();
    }
    const GPUVAddr gpu_addr_end = native ? gpu_addr_begin + native_size : limit.Address() + 1;
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    u32 size = address_size; // TODO: Analyze stride and number of vertices
    if (array.enable == 0 || size == 0 || !device_addr) {
        channel_state->vertex_buffers[index] = NULL_BINDING;
        UpdateVertexBufferSlot(index, NULL_BINDING);
        return;
    }
    if (!gpu_memory->IsWithinGPUAddressRange(gpu_addr_end) || size >= 64_MiB) {
        size = static_cast<u32>(gpu_memory->MaxContinuousRange(gpu_addr_begin, size));
    }
    const BufferId buffer_id = FindBuffer(*device_addr, size, false);
    const Binding binding{
        .device_addr = *device_addr,
        .size = size,
        .buffer_id = buffer_id,
    };
    if (Nx2Profile::Enabled()) {
        const Binding& previous = channel_state->vertex_buffers[index];
        Nx2Profile::Add(Nx2Profile::Counter::VertexDirty, 1);
        Nx2Profile::Add(Nx2Profile::Counter::VertexSame,
                        previous.device_addr == binding.device_addr &&
                                previous.size == binding.size &&
                                previous.buffer_id == binding.buffer_id
                            ? 1
                            : 0);
    }
    channel_state->vertex_buffers[index] = binding;
    UpdateVertexBufferSlot(index, binding);
}

template <class P>
void BufferCache<P>::UpdateDrawIndirect() {
    const auto update = [this](GPUVAddr gpu_addr, size_t size, Binding& binding) {
        const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        if (!device_addr) {
            binding = NULL_BINDING;
            return;
        }
        binding = Binding{
            .device_addr = *device_addr,
            .size = static_cast<u32>(size),
            .buffer_id = FindBuffer(*device_addr, static_cast<u32>(size), false),
        };
    };
    if (current_draw_indirect->include_count) {
        update(current_draw_indirect->count_start_address, sizeof(u32),
               channel_state->count_buffer_binding);
    }
    update(current_draw_indirect->indirect_start_address, current_draw_indirect->buffer_size,
           channel_state->indirect_buffer_binding);
}

template <class P>
void BufferCache<P>::UpdateUniformBuffers(size_t stage) {
    if (channel_state->enabled_uniform_buffer_masks[stage] == 0) [[likely]] {
        return;
    }
    ForEachEnabledBit(channel_state->enabled_uniform_buffer_masks[stage], [&](u32 index) {
        Binding& binding = channel_state->uniform_buffers[stage][index];
        if (binding.buffer_id) {
            // Already updated
            return;
        }
        // Mark as dirty
        if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
            channel_state->dirty_uniform_buffers[stage] |= 1U << index;
        }
        // Resolve buffer
        binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
    });
}

template <class P>
void BufferCache<P>::UpdateStorageBuffers(size_t stage) {
    if (channel_state->enabled_storage_buffers[stage] == 0) [[likely]] {
        return;
    }
    ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
        // Resolve buffer
        Binding& binding = channel_state->storage_buffers[stage][index];
        const BufferId buffer_id = FindBuffer(binding.device_addr, binding.size, false);
        binding.buffer_id = buffer_id;
        const bool is_written = ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0;
        ResolveMultiRangeStorage(binding, is_written, graphics_segments);
    });
}

template <class P>
void BufferCache<P>::UpdateTextureBuffers(size_t stage) {
    if (channel_state->enabled_texture_buffers[stage] == 0) [[likely]] {
        return;
    }
    ForEachEnabledBit(channel_state->enabled_texture_buffers[stage], [&](u32 index) {
        Binding& binding = channel_state->texture_buffers[stage][index];
        binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
    });
}

template <class P>
void BufferCache<P>::UpdateTransformFeedbackBuffers() {
    if (maxwell3d->regs.transform_feedback_enabled == 0) {
        return;
    }
    for (u32 index = 0; index < NUM_TRANSFORM_FEEDBACK_BUFFERS; ++index) {
        UpdateTransformFeedbackBuffer(index);
    }
}

template <class P>
void BufferCache<P>::UpdateTransformFeedbackBuffer(u32 index) {
    const auto& binding = maxwell3d->regs.transform_feedback.buffers[index];
    const GPUVAddr gpu_addr = binding.Address() + binding.start_offset;
    const u32 size = binding.size;
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (binding.enable == 0 || size == 0 || !device_addr) {
        channel_state->transform_feedback_buffers[index] = NULL_BINDING;
        return;
    }
    const BufferId buffer_id = FindBuffer(*device_addr, size, false);
    channel_state->transform_feedback_buffers[index] = Binding{
        .device_addr = *device_addr,
        .size = size,
        .buffer_id = buffer_id,
    };
}

template <class P>
void BufferCache<P>::UpdateComputeUniformBuffers() {
    ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
        Binding& binding = channel_state->compute_uniform_buffers[index];
        binding = NULL_BINDING;
        const auto& launch_desc = kepler_compute->launch_description;
        if (((launch_desc.const_buffer_enable_mask >> index) & 1) != 0) {
            const auto& cbuf = launch_desc.const_buffer_config[index];
            const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(cbuf.Address());
            if (device_addr) {
                binding.device_addr = *device_addr;
                binding.size = cbuf.size;
            }
        }
        binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
    });
}

template <class P>
void BufferCache<P>::UpdateComputeStorageBuffers() {
    ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
        // Resolve buffer
        Binding& binding = channel_state->compute_storage_buffers[index];
        binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
        const bool is_written =
            ((channel_state->written_compute_storage_buffers >> index) & 1) != 0;
        ResolveMultiRangeStorage(binding, is_written, compute_segments);
    });
}

template <class P>
void BufferCache<P>::UpdateComputeTextureBuffers() {
    ForEachEnabledBit(channel_state->enabled_compute_texture_buffers, [&](u32 index) {
        Binding& binding = channel_state->compute_texture_buffers[index];
        binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
    });
}

template <class P>
void BufferCache<P>::NoteGpuWrite(Buffer& buffer, DAddr device_addr, u64 size) {
    if constexpr (!IS_OPENGL) {
        const u64 tick = runtime.CurrentTick();
        buffer.SetGpuWriteTick(tick);
        if (Tegra::Nx2GpuWriteTicks()) {
            Tegra::Nx2NoteGpuWriteTick(device_addr, size, tick);
        }
    }
}

template <class P>
void BufferCache<P>::MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size, bool needs_sync) {
    Nx2Profile::Scope profile_scope{Nx2Profile::Zone::MarkWritten};
    if constexpr (!IS_OPENGL) {
        Buffer& buffer = slot_buffers[buffer_id];
        NoteGpuWrite(buffer, device_addr, size);
        if (Tegra::Nx2ClSpeculate()) {
            // A dispatch's writes get the signature of its inputs once all are bound.
            if (nx2_collect_list_copies) {
                nx2_pending_writes.emplace_back(device_addr, size);
            } else {
                Tegra::Nx2SetRangeUnique(device_addr, size, runtime.CurrentTick());
            }
        }
        if (needs_sync) {
            buffer.setWriteTick(runtime.CurrentTick());
        }
    }
    // The downloads already taken over this range hold older bytes: when their fences pop
    // they must not write them over this write's result in guest memory, nor drop the range
    // from gpu_modified_ranges (the next fence downloads it instead).
    for (auto& ranges : pending_download_ranges) {
        ranges.Subtract(device_addr, size);
    }
    memory_tracker.MarkRegionAsGpuModified(device_addr, size);
    MarkVertexBuffersStale(device_addr, size);
    gpu_modified_ranges.Add(device_addr, size);
    QueueFenceDownload(device_addr, size);
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        Tegra::Nx2MarkGpuWritten(device_addr, size);
        Nx2NoteListWrite(buffer_id, device_addr, size);
    }
}

template <class P>
void BufferCache<P>::Nx2NoteListWrite(BufferId buffer_id, DAddr device_addr, u64 size) {
    if constexpr (!IS_OPENGL) {
        if (size == 0 || !Tegra::g_nx2_has_command_list_pages.load(std::memory_order_relaxed)) {
            return;
        }
        constexpr u64 page_bits = Tegra::Nx2WrittenPageBits;
        const u64 last = (std::min<u64>)((device_addr + size - 1) >> page_bits,
                                         Tegra::g_nx2_command_list_pages.size() * 64 - 1);
        for (u64 page = device_addr >> page_bits; page <= last;) {
            // Storage buffers can be large: skip 64 pages at a time where none is a list page.
            const u64 word =
                Tegra::g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >>
                (page % 64);
            if (word == 0) {
                page = (page / 64 + 1) * 64;
                continue;
            }
            if (word & 1) {
                const DAddr page_addr = page << page_bits;
                {
                    std::scoped_lock lock{nx2_list_mutex};
                    Nx2ListPage& entry = nx2_list_pages[page_addr];
                    ++entry.writes;
                    if (!nx2_collect_list_copies && !entry.draw_written) {
                        entry.draw_written = true;
                        LOG_INFO(HW_GPU, "Command list page {:#x} is written by draws too: its "
                                         "lists drain the split",
                                 page_addr);
                    }
                }
                if (nx2_collect_list_copies) {
                    nx2_list_copy_requests.emplace_back(buffer_id, page_addr);
                }
            }
            ++page;
        }
    }
}

template <class P>
bool BufferCache<P>::Nx2CopyListPages(u64 tick) {
    if constexpr (IS_OPENGL) {
        return false;
    } else {
        if (nx2_list_copy_requests.empty()) {
            return false;
        }
        std::ranges::sort(nx2_list_copy_requests, {}, &std::pair<BufferId, DAddr>::second);
        constexpr DAddr page_size = DAddr{1} << Tegra::Nx2WrittenPageBits;
        bool recorded = false;
        for (size_t i = 0; i < nx2_list_copy_requests.size(); ++i) {
            const auto [buffer_id, page] = nx2_list_copy_requests[i];
            if (i + 1 < nx2_list_copy_requests.size() &&
                nx2_list_copy_requests[i + 1].second == page) {
                continue;
            }
            Buffer& buffer = slot_buffers[buffer_id];
            const DAddr begin = (std::max<DAddr>)(page, buffer.CpuAddr());
            const DAddr end =
                (std::min<DAddr>)(page + page_size, buffer.CpuAddr() + buffer.SizeBytes());
            if (begin >= end) {
                continue;
            }
            Nx2Staging staging = runtime.DownloadStagingBuffer(page_size, true);
            const BufferCopy copy{
                .src_offset = buffer.Offset(begin),
                .dst_offset = staging.offset + (begin - page),
                .size = end - begin,
            };
            buffer.MarkUsage(copy.src_offset, copy.size);
            runtime.CopyBuffer(staging.buffer, buffer, std::span{&copy, 1}, true);
            recorded = true;
            std::scoped_lock lock{nx2_list_mutex};
            Nx2ListPage& entry = nx2_list_pages[page];
            if (entry.copy) {
                nx2_retired_list_copies.emplace_back(entry.copy_tick, std::move(*entry.copy));
            }
            entry.copy = staging;
            entry.copy_writes = entry.writes;
            entry.copy_tick = tick;
            entry.global_writer = nx2_writer_global;
            entry.copy_begin = static_cast<u32>(begin - page);
            entry.copy_end = static_cast<u32>(end - page);
        }
        nx2_list_copy_requests.clear();
        std::erase_if(nx2_retired_list_copies, [this](auto& retired) {
            if (!runtime.IsFree(retired.first)) {
                return false;
            }
            runtime.FreeDeferredStagingBuffer(retired.second);
            return true;
        });
        return recorded;
    }
}

template <class P>
auto BufferCache<P>::Nx2ReadListCopy(DAddr device_addr, std::span<u8> out, u64* pending_tick)
    -> Nx2ListRead {
    if constexpr (IS_OPENGL) {
        return Nx2ListRead::Unknown;
    } else {
        constexpr DAddr page_size = DAddr{1} << Tegra::Nx2WrittenPageBits;
        const DAddr page = device_addr & ~(page_size - 1);
        const u64 offset = device_addr - page;
        if (offset + out.size() > page_size) {
            return Nx2ListRead::Unknown;
        }
        const auto current = [&](const Nx2ListPage& entry) {
            return entry.copy && !entry.draw_written && entry.copy_writes == entry.writes &&
                   offset >= entry.copy_begin && offset + out.size() <= entry.copy_end;
        };
        // EDEN_NX2_STRATO_STATS: whether the words a read waited for are those the read of the
        // same words before got (a write that changed nothing there: the bind covers the page).
        const bool stats = Tegra::Nx2StratoStats();
        const auto same_words = [&](Nx2ListPage& entry) {
            const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(out.data()), out.size());
            const bool same = entry.read_offset == offset && entry.read_size == out.size() &&
                              entry.read_hash == hash;
            entry.read_offset = static_cast<u32>(offset);
            entry.read_size = static_cast<u32>(out.size());
            entry.read_hash = hash;
            return same;
        };
        // EDEN_NX2_CL_SPECULATE=measure: the words' content signature before any wait, and
        // whether it predicted that they are those of the read before (nx2_speculate.h).
        const bool speculate = Tegra::Nx2ClSpeculate() != 0;
        const u64 content_sig = speculate ? Tegra::Nx2RangeSig(device_addr, out.size()) : 0;
        const auto note_speculation = [&](Nx2ListPage& entry, bool waited, u64 wait_ns) {
            if (!speculate) {
                return;
            }
            Tegra::Nx2SignTimer timer;
            const u64 hash =
                Common::CityHash64(reinterpret_cast<const char*>(out.data()), out.size());
            auto& spans = entry.spans;
            const auto span = std::ranges::find_if(spans, [&](const Nx2ListPage::Span& s) {
                return s.offset == offset && s.size == out.size();
            });
            if (span == spans.end()) {
                if (spans.size() >= 8) {
                    spans.erase(spans.begin());
                }
                spans.push_back({static_cast<u32>(offset), static_cast<u32>(out.size()),
                                 content_sig, hash});
                return;
            }
            auto& stats = Tegra::g_nx2_speculation;
            stats.global.fetch_add(entry.global_writer ? 1 : 0, std::memory_order_relaxed);
            const bool predicted = span->sig == content_sig;
            const bool same = span->hash == hash;
            stats.reads.fetch_add(1, std::memory_order_relaxed);
            stats.waits.fetch_add(waited ? 1 : 0, std::memory_order_relaxed);
            stats.same.fetch_add(same ? 1 : 0, std::memory_order_relaxed);
            if (predicted) {
                stats.predicted.fetch_add(1, std::memory_order_relaxed);
                stats.predicted_waits.fetch_add(waited ? 1 : 0, std::memory_order_relaxed);
                (same ? stats.right : stats.wrong).fetch_add(1, std::memory_order_relaxed);
                if (waited && same) {
                    stats.waited_ns.fetch_add(wait_ns, std::memory_order_relaxed);
                }
            }
            span->sig = content_sig;
            span->hash = hash;
            Tegra::Nx2ReportSpeculation();
        };
        u64 tick{};
        u64 writes{};
        {
            std::scoped_lock lock{nx2_list_mutex};
            const auto it = nx2_list_pages.find(page);
            if (it == nx2_list_pages.end()) {
                return Nx2ListRead::Unknown;
            }
            const Nx2ListPage& entry = it->second;
            // Pages the CPU writes the lists of (GPU-written once, long before) stay synced.
            if (!entry.draw_written && entry.synced_writes == entry.writes) {
                return Nx2ListRead::Synced;
            }
            if (!current(entry)) {
                return Nx2ListRead::Unknown;
            }
            runtime.NoteListCopyRead();
            // The lists after the first of a batch find the copy done: read it at once.
            if (runtime.IsFree(entry.copy_tick)) {
                std::memcpy(out.data(), entry.copy->mapped_span.data() + offset, out.size());
                if (stats) {
                    same_words(it->second);
                }
                note_speculation(it->second, false, 0);
                return Nx2ListRead::Copied;
            }
            tick = entry.copy_tick;
            writes = entry.copy_writes;
            if (pending_tick != nullptr) {
                runtime.NoteListCopyPending();
                *pending_tick = tick;
                return Nx2ListRead::Pending;
            }
        }
        // EDEN_NX2_GPU_DIRTY_GEN: no wait when the GPU is past the copy (asked, not remembered),
        // or, without the split (whose back owns the buffers), past the last write to the page:
        // the buffer's memory holds the words then.
        if (Tegra::Nx2GpuDirtyGen()) {
            const bool copy_done = runtime.TickComplete(tick);
            if (copy_done ||
                (!VideoCommon::GpuSplit::Get().Active() && Nx2ReadListHost(device_addr, out))) {
                std::scoped_lock lock{nx2_list_mutex};
                const auto it = nx2_list_pages.find(page);
                if (it == nx2_list_pages.end() || !current(it->second) ||
                    it->second.copy_tick != tick || it->second.copy_writes != writes) {
                    return Nx2ListRead::Unknown;
                }
                if (copy_done) {
                    std::memcpy(out.data(), it->second.copy->mapped_span.data() + offset,
                                out.size());
                }
                if (stats) {
                    same_words(it->second);
                    VideoCommon::Nx2Strato::NoteGenSkip(VideoCommon::Nx2Strato::Wait::ListCopy);
                }
                note_speculation(it->second, false, 0);
                return Nx2ListRead::Copied;
            }
        }
        // Whether the last GPU write to the page was done already (only its copy was not).
        const u64 write_tick = stats ? Tegra::Nx2LastGpuWriteTick(device_addr, out.size()) : 0;
        const bool write_done = write_tick != 0 && runtime.TickComplete(write_tick);
        const u64 wait_start = stats || speculate ? VideoCommon::Nx2Strato::Now() : 0;
        // Submitted with the work that wrote the page (Nx2CopyListPages's caller flushes).
        runtime.WaitSubmittedTick(tick);
        const u64 waited = stats || speculate ? VideoCommon::Nx2Strato::Now() - wait_start : 0;
        std::scoped_lock lock{nx2_list_mutex};
        const auto it = nx2_list_pages.find(page);
        if (it == nx2_list_pages.end() || !current(it->second) || it->second.copy_tick != tick ||
            it->second.copy_writes != writes) {
            if (stats) {
                VideoCommon::Nx2Strato::NoteWait(VideoCommon::Nx2Strato::Wait::ListCopy, waited,
                                                 write_done);
            }
            return Nx2ListRead::Unknown;
        }
        std::memcpy(out.data(), it->second.copy->mapped_span.data() + offset, out.size());
        note_speculation(it->second, true, waited);
        if (stats) {
            if (same_words(it->second)) {
                VideoCommon::Nx2Strato::NoteSameWords(waited);
            }
            VideoCommon::Nx2Strato::NoteWait(VideoCommon::Nx2Strato::Wait::ListCopy, waited,
                                             write_done);
        }
        return Nx2ListRead::Copied;
    }
}

template <class P>
bool BufferCache<P>::Nx2ReadListHost(DAddr device_addr, std::span<u8> out) {
    if constexpr (requires { runtime.CanReadBuffersDirectly(); std::declval<Buffer&>().HostView(); }) {
        if (!runtime.CanReadBuffersDirectly()) {
            return false;
        }
        std::scoped_lock lock{mutex};
        const BufferId buffer_id = page_table[device_addr >> CACHING_PAGEBITS];
        if (!buffer_id) {
            return false;
        }
        Buffer& buffer = slot_buffers[buffer_id];
        // Every write to the buffer notes its pages (NoteGpuWrite): the page's tick covers them.
        const u64 tick = Tegra::Nx2LastGpuWriteTick(device_addr, out.size());
        if (!buffer.IsInBounds(device_addr, out.size()) || buffer.GpuWriteTick() == 0 ||
            tick == 0 || !runtime.TickComplete(tick)) {
            return false;
        }
        const std::span<const u8> host = buffer.HostView();
        if (host.empty()) {
            return false;
        }
        buffer.InvalidateHostView();
        std::memcpy(out.data(), host.data() + buffer.Offset(device_addr), out.size());
        return true;
    } else {
        return false;
    }
}

template <class P>
void BufferCache<P>::Nx2NoteListSynced(DAddr page) {
    if constexpr (!IS_OPENGL) {
        std::scoped_lock lock{nx2_list_mutex};
        Nx2ListPage& entry = nx2_list_pages[page];
        entry.synced_writes = entry.writes;
    }
}

template <class P>
void BufferCache<P>::Nx2NoteListHostWrite(DAddr device_addr, u64 size) {
    if constexpr (!IS_OPENGL) {
        if (size == 0 || !Tegra::g_nx2_has_command_list_pages.load(std::memory_order_relaxed)) {
            return;
        }
        constexpr u64 page_bits = Tegra::Nx2WrittenPageBits;
        const u64 last = (std::min<u64>)((device_addr + size - 1) >> page_bits,
                                         Tegra::g_nx2_command_list_pages.size() * 64 - 1);
        for (u64 page = device_addr >> page_bits; page <= last; ++page) {
            if (!((Tegra::g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >>
                   (page % 64)) &
                  1)) {
                continue;
            }
            std::scoped_lock lock{nx2_list_mutex};
            const auto it = nx2_list_pages.find(page << page_bits);
            if (it == nx2_list_pages.end()) {
                continue;
            }
            Nx2ListPage& entry = it->second;
            const bool synced = entry.synced_writes == entry.writes;
            ++entry.writes;
            if (synced) {
                entry.synced_writes = entry.writes;
            }
        }
    }
}

template <class P>
u64 BufferCache<P>::Nx2ComputeInputSig() {
    using Tegra::Nx2SigMix;
    u64 sig = 0x636f6d70ULL;
    // Uniform buffers by their bytes in guest memory: they may be read from there (streamed).
    ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
        const Binding& binding = channel_state->compute_uniform_buffers[index];
        const u32 size =
            (std::min)(binding.size, (*channel_state->compute_uniform_buffer_sizes)[index]);
        sig = Nx2SigMix(Nx2SigMix(sig, index), binding.device_addr);
        if (binding.device_addr != 0 && size != 0) {
            tmp_buffer.resize_destructive(size);
            device_memory.ReadBlockUnsafe(binding.device_addr, tmp_buffer.data(), size);
            sig = Nx2SigMix(sig, Common::CityHash64(reinterpret_cast<const char*>(tmp_buffer.data()),
                                                    size));
        }
    });
    ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
        const Binding& binding = channel_state->compute_storage_buffers[index];
        sig = Nx2SigMix(Nx2SigMix(Nx2SigMix(sig, 0x100 + index), binding.device_addr),
                        Tegra::Nx2RangeSig(binding.device_addr, binding.size));
    });
    ForEachEnabledBit(channel_state->enabled_compute_texture_buffers, [&](u32 index) {
        const Binding& binding = channel_state->compute_texture_buffers[index];
        sig = Nx2SigMix(Nx2SigMix(Nx2SigMix(sig, 0x200 + index), binding.device_addr),
                        Tegra::Nx2RangeSig(binding.device_addr, binding.size));
    });
    return sig;
}

template <class P>
void BufferCache<P>::Nx2FinishDispatchSig(u64 writer) {
    for (const auto& [device_addr, size] : nx2_pending_writes) {
        Tegra::Nx2SetRangeSig(device_addr, size, Tegra::Nx2SigMix(writer, device_addr));
    }
    nx2_pending_writes.clear();
}

template <class P>
BufferId BufferCache<P>::FindBuffer(DAddr device_addr, u32 size, bool sparse_compatible) {
    const u64 page = device_addr >> CACHING_PAGEBITS;
    if (device_addr == 0 || page >= page_table.size()) {
        return NULL_BUFFER_ID;
    }
    const BufferId buffer_id = page_table[page];
    if (buffer_id) {
        Buffer& buffer = slot_buffers[buffer_id];
        if (buffer.IsInBounds(device_addr, size)) {
            const bool should_sync = Settings::IsGPUFenceBehaviorAccurate();
            if (should_sync) {
                SynchronizeBufferWrites(buffer);
            }
            bool usable = true;
            if constexpr (requires { buffer.IsSparseCompatible(); }) {
                if (sparse_compatible && !buffer.IsSparseCompatible()) {
                    usable = false;
                }
            }
            if (usable) {
                return buffer_id;
            }
        }
    }
    return CreateBuffer(device_addr, size, sparse_compatible);
}

template <class P>
void BufferCache<P>::SynchronizeBufferWrites(Buffer& buffer) {
    if constexpr (!IS_OPENGL) {
        const u64 buffer_tick = buffer.getWriteTick();
        if (!runtime.IsFree(buffer_tick)) {
            runtime.Wait(buffer_tick);
        }
    }
}

template <class P>
typename BufferCache<P>::OverlapResult BufferCache<P>::ResolveOverlaps(DAddr device_addr,
                                                                       u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    DAddr begin = device_addr;
    DAddr end = device_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    auto expand_begin = [&](DAddr add_value) {
        static constexpr DAddr min_page = CACHING_PAGESIZE + Core::DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = Core::DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    auto expand_end = [&](DAddr add_value) {
        static constexpr DAddr max_page = 1ULL << Tegra::MaxwellDeviceMemoryManager::AS_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS];
        if (!overlap_id) {
            continue;
        }
        if (!slot_buffers.contains(overlap_id)) [[unlikely]] {
            // A deleted buffer is still in the page table. Joining it would delete whichever
            // buffer reuses the slot next, possibly the one being created here.
            static u32 stale_logs = 0;
            if (stale_logs < 16) {
                ++stale_logs;
                LOG_ERROR(HW_GPU, "Stale buffer page: device_addr={:#x} buffer_id={}",
                          device_addr, overlap_id.index);
            }
            page_table[device_addr >> CACHING_PAGEBITS] = BufferId{};
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.IsPicked()) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.Pick();
        const DAddr overlap_device_addr = overlap.CpuAddr();
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const DAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            if (expands_right) {
                expand_begin(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_end(CACHING_PAGESIZE * 128);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

template <class P>
void BufferCache<P>::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                                 bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    boost::container::small_vector<BufferCopy, 10> copies;
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    copies.push_back(BufferCopy{
        .src_offset = 0,
        .dst_offset = dst_base_offset,
        .size = overlap.SizeBytes(),
    });
    new_buffer.MarkUsage(copies[0].dst_offset, copies[0].size);
    NoteGpuWrite(new_buffer, overlap.CpuAddr(), overlap.SizeBytes());
    runtime.CopyBuffer(new_buffer, overlap, copies, true);
#ifdef YUZU_LEGACY
    if (immediately_free)
        runtime.Finish();
#endif
    DeleteBuffer(overlap_id, true);
}

template <class P>
BufferId BufferCache<P>::CreateBuffer(DAddr device_addr, u32 wanted_size,
                                      bool sparse_compatible) {
    DAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id =
        slot_buffers.insert(runtime, overlap.begin, size, sparse_compatible);
    auto& new_buffer = slot_buffers[new_buffer_id];
    const size_t size_bytes = new_buffer.SizeBytes();
    NoteGpuWrite(new_buffer, new_buffer.CpuAddr(), size_bytes);
    runtime.ClearBuffer(new_buffer, 0, size_bytes, 0);
    new_buffer.MarkUsage(0, size_bytes);
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    // Hot pages the joined buffers didn't hold are only zeros here.
    memory_tracker.InvalidateHotShadows(new_buffer.CpuAddr(), size_bytes);
    Register(new_buffer_id);
    TouchBuffer(new_buffer, new_buffer_id);
    return new_buffer_id;
}

template <class P>
void BufferCache<P>::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
}

template <class P>
void BufferCache<P>::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
}

template <class P>
template <bool insert>
void BufferCache<P>::ChangeRegister(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    if (insert) {
        buffer_bytes += Common::AlignUp(size, 1024);
        total_used_memory += Common::AlignUp(size, 1024);
        buffer.setLRUID(lru_cache.Insert(buffer_id, frame_tick));
    } else {
        buffer_bytes -= Common::AlignUp(size, 1024);
        total_used_memory -= Common::AlignUp(size, 1024);
        // Only a registered buffer has an LRU item (freeing SIZE_MAX crashed in Free).
        if (buffer.getLRUID() != SIZE_MAX) {
            lru_cache.Free(buffer.getLRUID());
            buffer.setLRUID(SIZE_MAX);
        }
    }
    const DAddr device_addr_begin = buffer.CpuAddr();
    const DAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page] = buffer_id;
        } else if (page_table[page] == buffer_id) {
            // Only its own entries: another buffer's would go stale.
            page_table[page] = BufferId{};
        }
    }
}

template <class P>
void BufferCache<P>::TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept {
    if (buffer_id != NULL_BUFFER_ID && buffer.getLRUID() != SIZE_MAX) {
        lru_cache.Touch(buffer.getLRUID(), frame_tick);
    }
}

/// Upload CPU-dirty pages in the range. A range with no pending CPU or
/// untracked bits is already synchronized.
template <class P>
bool BufferCache<P>::SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size) {
    Nx2Profile::Scope profile_scope{Nx2Profile::Zone::SyncBuffer};
    Nx2Profile::Add(Nx2Profile::Counter::SyncCalls, 1);
    // A vertex, index, or storage buffer the guest has not written since its last upload
    // has a clean dirty bitmap. Walking that bitmap on every later draw is the bulk of
    // the "update buffers" time (#562).
    if (!memory_tracker.HasPendingCpuUpload(device_addr, size)) {
        static u64 skips = 0;
        ++skips;
        if (skips == 1 || skips == 100000 || skips % 1000000 == 0) {
            LOG_INFO(HW_GPU, "Clean buffer uploads skipped {}", skips);
        }
        return true;
    }
    // Most binds come back to a range nothing was written to since it was synchronized: skip
    // walking its pages while its region saw no CPU writes (they took 5% of Mario Kart World's
    // GPU back thread).
    // EDEN_NX2_SYNC_CACHE=0 synchronizes every range every time. Android only: desktop
    // synchronizes every range every time as before.
#ifdef __ANDROID__
    static const bool sync_cache = [] {
        const char* value = std::getenv("EDEN_NX2_SYNC_CACHE");
        return value == nullptr || value[0] != '0';
    }();
#else
    constexpr bool sync_cache = false;
#endif
    const u64 generation = sync_cache ? memory_tracker.RangeGeneration(device_addr, size) : 0;
    SyncedRange& synced = synced_ranges[((device_addr >> 8) ^ size) % synced_ranges.size()];
    if (generation != 0 && synced.generation == generation && synced.device_addr == device_addr &&
        synced.size == size && synced.buffer == &buffer) {
        return true;
    }
    if (generation != 0) {
        synced = SyncedRange{&buffer, device_addr, size, generation};
    }
    Nx2Profile::Add(Nx2Profile::Counter::SyncWalks, 1);
    // EDEN_NX2_HOT_PAGES: hot pages first (one that goes back to write tracking here is then
    // uploaded by the walk below).
    bool hot_uploaded = false;
    if (memory_tracker.HasHotPages()) {
        hot_uploads.clear();
        memory_tracker.ForEachHotUpload(device_addr, size, buffer.CpuAddr(),
                                        buffer.CpuAddr() + buffer.SizeBytes(),
                                        [&](DAddr address, const u8* bytes, u64 bytes_size) {
                                            hot_uploads.push_back({address, bytes, bytes_size});
                                        });
        if (!hot_uploads.empty()) {
            UploadHotPages(buffer, hot_uploads);
            hot_uploaded = true;
        }
    }
#ifdef __ANDROID__
    const bool use_optimized_vertex_buffers = Settings::values.use_optimized_vertex_buffers.GetValue();
#else
    constexpr bool use_optimized_vertex_buffers = true;
#endif

    if (use_optimized_vertex_buffers) {
        upload_copies.clear();
        u64 total_size_bytes = 0;
        u64 largest_copy = 0;
        const DAddr buffer_start = buffer.cpu_addr_cached;
        const auto add_upload = [&](DAddr start, DAddr end) {
            if (start == end) return;
            const u64 range_size = end - start;
            upload_copies.push_back(BufferCopy{
                .src_offset = total_size_bytes,
                .dst_offset = start - buffer_start,
                .size = range_size,
            });
            total_size_bytes += range_size;
            largest_copy = (std::max)(largest_copy, range_size);
        };
        memory_tracker.ForEachUploadRange(device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            DAddr upload_start = device_addr_out;
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, [&](DAddr gpu_start, DAddr gpu_end) {
                add_upload(upload_start, gpu_start);
                upload_start = gpu_end;
            });
            add_upload(upload_start, device_addr_out + range_size);
        });
        if (total_size_bytes == 0) {
            return !hot_uploaded;
        }
        const std::span<BufferCopy> copies_span(upload_copies.data(), upload_copies.size());
        UploadMemory(buffer, total_size_bytes, largest_copy, copies_span);
        // Mapped uploads carry their own barriers (or run in the upload command buffer, which
        // ends with one); another barrier here only ended the render pass (~50 times a frame in
        // Pokopia).
        if constexpr (!USE_MEMORY_MAPS_FOR_UPLOADS) {
            any_buffer_uploaded = true;
        }
        return false;
    } else {
        boost::container::small_vector<BufferCopy, 4> copies;
        u64 total_size_bytes = 0;
        u64 largest_copy = 0;
        DAddr buffer_start = buffer.CpuAddr();
        memory_tracker.ForEachUploadRange(device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            copies.push_back(BufferCopy{
                .src_offset = total_size_bytes,
                .dst_offset = device_addr_out - buffer_start,
                .size = range_size,
            });
            total_size_bytes += range_size;
            largest_copy = (std::max)(largest_copy, range_size);
        });
        if (total_size_bytes == 0) {
            return !hot_uploaded;
        }
        const std::span<BufferCopy> copies_span(copies.data(), copies.size());
        UploadMemory(buffer, total_size_bytes, largest_copy, copies_span);
        return false;
    }
}

template <class P>
void BufferCache<P>::UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                                  std::span<BufferCopy> copies) {
    if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
        MappedUploadMemory(buffer, total_size_bytes, copies);
    } else {
        ImmediateUploadMemory(buffer, largest_copy, copies);
    }
}

template <class P>
void BufferCache<P>::ImmediateUploadMemory([[maybe_unused]] Buffer& buffer,
                                           [[maybe_unused]] u64 largest_copy,
                                           [[maybe_unused]] std::span<const BufferCopy> copies) {
    if constexpr (!USE_MEMORY_MAPS_FOR_UPLOADS) {
        std::span<u8> immediate_buffer;
        for (const BufferCopy& copy : copies) {
            std::span<const u8> upload_span;
            const DAddr device_addr = buffer.CpuAddr() + copy.dst_offset;
            if (IsRangeGranular(device_addr, copy.size)) {
                auto* const ptr = device_memory.GetPointer<u8>(device_addr);
                if (ptr != nullptr) {
                    upload_span = std::span(ptr, copy.size);
                }
            } else {
                if (immediate_buffer.empty()) {
                    immediate_buffer = ImmediateBuffer(largest_copy);
                }
                device_memory.ReadBlockUnsafe(device_addr, immediate_buffer.data(), copy.size);
                upload_span = immediate_buffer.subspan(0, copy.size);
            }
            buffer.ImmediateUpload(copy.dst_offset, upload_span);
        }
    }
}

template <class P>
void BufferCache<P>::MappedUploadMemory([[maybe_unused]] Buffer& buffer,
                                        [[maybe_unused]] u64 total_size_bytes,
                                        [[maybe_unused]] std::span<BufferCopy> copies) {
    if constexpr (USE_MEMORY_MAPS) {
        auto upload_staging = runtime.UploadStagingBuffer(total_size_bytes);
        const std::span<u8> staging_pointer = upload_staging.mapped_span;
        for (BufferCopy& copy : copies) {
            u8* const src_pointer = staging_pointer.data() + copy.src_offset;
            const DAddr device_addr = buffer.CpuAddr() + copy.dst_offset;
            device_memory.ReadBlockUnsafe(device_addr, src_pointer, copy.size);
            if (Tegra::Nx2ClSpeculate()) {
                Tegra::Nx2NoteUploadSig(device_addr, std::span<const u8>(src_pointer, copy.size));
            }
            // Large uploads are mostly geometry the host GPU now has its own copy of: TickFrame
            // marks their guest memory cold (see TextureCache::RefreshContents).
            if (copy.size >= 256_KiB) {
                uploaded_buffer_ranges.emplace_back(device_addr, copy.size);
            }
            // Apply the staging offset
            copy.src_offset += upload_staging.offset;
        }
        const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
        // EDEN_NX2_PASS_BREAKS=1: uploads that go ahead of the command buffer against those that
        // must stay in order (their target was used in it: each ends the render pass), by size.
        static const bool upload_stats = [] {
            const char* const value = std::getenv("EDEN_NX2_PASS_BREAKS");
            return value != nullptr && value[0] == '1';
        }();
        if (upload_stats) [[unlikely]] {
            struct Stats {
                u64 uploads{};
                u64 bytes{};
                u64 ordered{};
                u64 ordered_bytes{};
                u64 ordered_copies{};
                std::array<u64, 6> sizes{}; // of ordered uploads: <=256, 1K, 4K, 16K, 64K, more
                std::array<u64, 4> buffers{}; // of ordered uploads: buffer size <=64K, 1M, 16M, more
            };
            static Stats stats;
            static auto window_start = std::chrono::steady_clock::now();
            ++stats.uploads;
            stats.bytes += total_size_bytes;
            if (!can_reorder) {
                ++stats.ordered;
                stats.ordered_bytes += total_size_bytes;
                stats.ordered_copies += copies.size();
                const u64 size = total_size_bytes;
                ++stats.sizes[size <= 256 ? 0 : size <= 1024 ? 1 : size <= 4096 ? 2 : size <= 16384 ? 3 : size <= 65536 ? 4 : 5];
                const u64 whole = buffer.SizeBytes();
                ++stats.buffers[whole <= 65536 ? 0 : whole <= (1 << 20) ? 1 : whole <= (16 << 20) ? 2 : 3];
            }
            if (const auto now = std::chrono::steady_clock::now();
                now - window_start >= std::chrono::seconds{10}) {
                LOG_INFO(HW_GPU,
                         "Buffer uploads in 10 s: {} ({} KiB), {} in order ({} KiB, {} ranges): "
                         "sizes <=256 {} <=1K {} <=4K {} <=16K {} <=64K {} more {} | their "
                         "buffers <=64K {} <=1M {} <=16M {} more {}",
                         stats.uploads, stats.bytes >> 10, stats.ordered, stats.ordered_bytes >> 10,
                         stats.ordered_copies, stats.sizes[0], stats.sizes[1], stats.sizes[2],
                         stats.sizes[3], stats.sizes[4], stats.sizes[5], stats.buffers[0],
                         stats.buffers[1], stats.buffers[2], stats.buffers[3]);
                stats = {};
                window_start = now;
            }
        }
        for (const BufferCopy& copy : copies) {
            NoteGpuWrite(buffer, buffer.CpuAddr() + copy.dst_offset, copy.size);
        }
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true, can_reorder);
    }
}

template <class P>
void BufferCache<P>::UploadHotPages(Buffer& buffer, std::span<const HotUpload> pages) {
    u64 total_size = 0;
    for (const auto& [page, bytes, page_size] : pages) {
        MarkVertexBuffersStale(page, page_size);
        total_size += page_size;
    }
    if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
        auto upload_staging = runtime.UploadStagingBuffer(total_size);
        hot_copies.clear();
        u64 src_offset = 0;
        for (const auto& [page, bytes, page_size] : pages) {
            std::memcpy(upload_staging.mapped_span.data() + src_offset, bytes, page_size);
            if (Tegra::Nx2ClSpeculate()) {
                Tegra::Nx2NoteUploadSig(page, std::span<const u8>(bytes, page_size));
            }
            const u64 dst_offset = buffer.Offset(page);
            const u64 staging_offset = upload_staging.offset + src_offset;
            if (!hot_copies.empty() &&
                hot_copies.back().dst_offset + hot_copies.back().size == dst_offset &&
                hot_copies.back().src_offset + hot_copies.back().size == staging_offset) {
                hot_copies.back().size += page_size;
            } else {
                hot_copies.push_back(BufferCopy{
                    .src_offset = staging_offset,
                    .dst_offset = dst_offset,
                    .size = page_size,
                });
            }
            src_offset += page_size;
        }
        const std::span<BufferCopy> copies(hot_copies.data(), hot_copies.size());
        const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
        for (const BufferCopy& copy : copies) {
            NoteGpuWrite(buffer, buffer.CpuAddr() + copy.dst_offset, copy.size);
        }
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true, can_reorder);
    } else {
        for (const auto& [page, bytes, page_size] : pages) {
            buffer.ImmediateUpload(buffer.Offset(page), std::span<const u8>(bytes, page_size));
        }
        any_buffer_uploaded = true;
    }
}

template <class P>
void BufferCache<P>::TickHotPages() {
    // Never hot: Switch 2 GPU-written pages (command lists the GPU fills, culling results) and
    // the command list pages the front reads.
    memory_tracker.TickHotPages([](DAddr page) {
        const u64 index = page >> Tegra::Nx2WrittenPageBits;
        if (index < Tegra::g_nx2_gpu_written_pages.size() * 64 &&
            ((Tegra::g_nx2_gpu_written_pages[index / 64].load(std::memory_order_relaxed) >>
              (index % 64)) &
             1) != 0) {
            return false;
        }
        return !Tegra::Nx2IsCommandListPage(page);
    });
    struct Totals {
        u64 protects, protect_pages, unprotects, faults, compares, unchanged, uploads, promoted,
            demoted, refused, blocked, capped;
        u64 frames, bank_pages;
    };
    static auto last_time = std::chrono::steady_clock::now();
    static Totals last{};
    static u64 last_upload_bytes = 0;
    static u64 frames = 0;
    static bool announced = false;
    ++frames;
    if (!announced) {
        announced = true;
        LOG_INFO(HW_GPU,
                 "Hot pages on (EDEN_NX2_HOT_PAGES): hot after write faults in {} of 4 frames, "
                 "at most {} pages",
                 HotPages::MinFrames(), HotPages::MaxPages());
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    if (seconds < 10.0) {
        return;
    }
    const auto& shared = HotPages::g_shared;
    const auto& stats = memory_tracker.HotStats();
    const Totals totals{
        shared.protects.load(std::memory_order_relaxed),
        shared.protect_pages.load(std::memory_order_relaxed),
        shared.unprotects.load(std::memory_order_relaxed),
        shared.faults.load(std::memory_order_relaxed),
        stats.compares,
        stats.unchanged,
        stats.uploads,
        stats.promoted,
        stats.demoted,
        stats.refused,
        stats.blocked,
        stats.capped,
        frames,
        hot_bank_pages,
    };
    const auto rate = [seconds](u64 now_value, u64 last_value) {
        return static_cast<double>(now_value - last_value) / seconds;
    };
    const u64 compares = totals.compares - last.compares;
    const u64 ticks = (std::max<u64>)(totals.frames - last.frames, 1);
    LOG_INFO(HW_GPU,
             "Hot pages: {} hot (+{} -{} in {:.1f} s, {} refused, {:.0f} GPU-written candidates "
             "a frame, {} capped) | protects {:.0f}/s ({:.0f} pages/s), unprotects {:.0f}/s, "
             "write faults {:.0f}/s | hot syncs {:.0f}/s, {:.1f}% unchanged, extra upload {:.0f} "
             "KiB/s | bank re-uploads kept protected {:.0f} pages/s",
             memory_tracker.HotPageCount(), totals.promoted - last.promoted,
             totals.demoted - last.demoted, seconds, totals.refused - last.refused,
             static_cast<double>(totals.blocked - last.blocked) / static_cast<double>(ticks),
             totals.capped - last.capped, rate(totals.protects, last.protects),
             rate(totals.protect_pages, last.protect_pages), rate(totals.unprotects, last.unprotects),
             rate(totals.faults, last.faults), rate(totals.compares, last.compares),
             compares != 0 ? 100.0 * static_cast<double>(totals.unchanged - last.unchanged) /
                                 static_cast<double>(compares)
                           : 0.0,
             static_cast<double>(stats.upload_bytes - last_upload_bytes) / seconds / 1024.0,
             rate(totals.bank_pages, last.bank_pages));
    last = totals;
    last_upload_bytes = stats.upload_bytes;
    last_time = now;
}

template <class P>
bool BufferCache<P>::InlineMemory(DAddr dest_address, size_t copy_size,
                                  std::span<const u8> inlined_buffer) {
    const bool is_dirty = IsRegionRegistered(dest_address, copy_size);
    if (!is_dirty) {
        return false;
    }
    DAddr aligned_start = Common::AlignDown(dest_address, DEVICE_PAGESIZE);
    DAddr aligned_end = Common::AlignUp(dest_address + copy_size, DEVICE_PAGESIZE);
    if (!IsRegionGpuModified(aligned_start, aligned_end - aligned_start)) {
        return false;
    }

    InlineMemoryImplementation(dest_address, copy_size, inlined_buffer);

    return true;
}

template <class P>
void BufferCache<P>::InlineMemoryImplementation(DAddr dest_address, size_t copy_size,
                                                std::span<const u8> inlined_buffer) {
    ClearDownload(dest_address, copy_size);
    gpu_modified_ranges.Subtract(dest_address, copy_size);
    MarkVertexBuffersStale(dest_address, copy_size);

    BufferId buffer_id = FindBuffer(dest_address, static_cast<u32>(copy_size), false);
    auto& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, dest_address, static_cast<u32>(copy_size));
    memory_tracker.UpdateHotShadows(dest_address, inlined_buffer.first(copy_size));

    if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
        auto upload_staging = runtime.UploadStagingBuffer(copy_size);
        std::array copies{BufferCopy{
            .src_offset = upload_staging.offset,
            .dst_offset = buffer.Offset(dest_address),
            .size = copy_size,
        }};
        u8* const src_pointer = upload_staging.mapped_span.data();
        std::memcpy(src_pointer, inlined_buffer.data(), copy_size);
        if (Tegra::Nx2ClSpeculate()) {
            Tegra::Nx2NoteUploadSig(dest_address, inlined_buffer.first(copy_size));
        }
        const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
        NoteGpuWrite(buffer, dest_address, copy_size);
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true, can_reorder);
    } else {
        buffer.ImmediateUpload(buffer.Offset(dest_address), inlined_buffer.first(copy_size));
    }
}

template <class P>
void BufferCache<P>::DownloadBufferMemory(Buffer& buffer) {
    DownloadBufferMemory(buffer, buffer.CpuAddr(), buffer.SizeBytes());
}

template <class P>
void BufferCache<P>::DownloadBuffersMemory(std::span<const BufferId> buffer_ids) {
    struct Download {
        BufferId buffer_id;
        BufferCopy copy;
    };
    std::vector<Download> downloads;
    u64 total_size_bytes = 0;
    for (const BufferId buffer_id : buffer_ids) {
        Buffer& buffer = slot_buffers[buffer_id];
        const DAddr buffer_addr = buffer.CpuAddr();
        memory_tracker.ForEachDownloadRangeAndClear(
            buffer_addr, buffer.SizeBytes(), [&](u64 device_addr_out, u64 range_size) {
                gpu_modified_ranges.ForEachInRange(device_addr_out, range_size,
                                                   [&](DAddr start, DAddr end) {
                    downloads.push_back({buffer_id, BufferCopy{
                                                        .src_offset = start - buffer_addr,
                                                        .dst_offset = total_size_bytes,
                                                        .size = end - start,
                                                    }});
                    total_size_bytes += Common::AlignUp(end - start, 64);
                });
                ClearDownload(device_addr_out, range_size);
                gpu_modified_ranges.Subtract(device_addr_out, range_size);
            });
    }
    if (total_size_bytes == 0) {
        return;
    }
    auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes);
    for (Download& download : downloads) {
        download.copy.dst_offset += download_staging.offset;
        Buffer& buffer = slot_buffers[download.buffer_id];
        buffer.MarkUsage(download.copy.src_offset, download.copy.size);
        runtime.CopyBuffer(download_staging.buffer, buffer, std::span{&download.copy, 1}, true);
    }
    runtime.Finish();
    const u8* const mapped_memory = download_staging.mapped_span.data();
    const Common::RangeSet<DAddr> cpu_written = PendingCpuWriteRanges();
    for (const Download& download : downloads) {
        const DAddr device_addr = slot_buffers[download.buffer_id].CpuAddr() + download.copy.src_offset;
        WriteDownloadedMemory(cpu_written, device_addr,
                              mapped_memory + (download.copy.dst_offset - download_staging.offset),
                              download.copy.size);
    }
}

template <class P>
void BufferCache<P>::DownloadBufferMemory(Buffer& buffer, DAddr device_addr, u64 size) {
    const std::array ranges{std::pair<DAddr, u64>{device_addr, size}};
    DownloadBufferRanges(buffer, ranges);
}

template <class P>
void BufferCache<P>::BeginDownloadBatch() {
    download_batch = true;
    download_batch_thread = std::this_thread::get_id();
}

template <class P>
void BufferCache<P>::EndDownloadBatch() {
    download_batch = false;
    if constexpr (!IS_OPENGL) {
        if (batched_downloads.empty()) {
            return;
        }
        runtime.Finish();
        const Common::RangeSet<DAddr> cpu_written = PendingCpuWriteRanges();
        for (BatchedDownload& download : batched_downloads) {
            const u8* const mapped_memory = download.staging.mapped_span.data();
            for (const BufferCopy& copy : download.copies) {
                WriteDownloadedMemory(cpu_written, download.buffer_addr + copy.src_offset,
                                      mapped_memory + (copy.dst_offset - download.staging.offset),
                                      copy.size);
            }
            runtime.FreeDeferredStagingBuffer(download.staging);
        }
        batched_downloads.clear();
    }
}

template <class P>
void BufferCache<P>::DownloadBufferRanges(Buffer& buffer,
                                          std::span<const std::pair<DAddr, u64>> ranges) {
    boost::container::small_vector<BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    const DAddr buffer_addr = buffer.CpuAddr();
    const DAddr buffer_end = buffer_addr + buffer.SizeBytes();
    for (const auto& [range_addr, range_size_in] : ranges) {
        // Only this buffer's part: the tracker and gpu_modified_ranges are shared by every buffer,
        // so a range past the buffer gave copies outside it (offsets past its size, or below its
        // start) and cleared other buffers' GPU-written state without downloading it.
        const DAddr range_begin = (std::max)(range_addr, buffer_addr);
        const DAddr range_end = (std::min)(range_addr + range_size_in, buffer_end);
        if (range_begin >= range_end) {
            continue;
        }
        memory_tracker.ForEachDownloadRangeAndClear(
            range_begin, range_end - range_begin, [&](u64 device_addr_out, u64 range_size) {
                const auto add_download = [&](DAddr start, DAddr end) {
                    start = (std::max)(start, buffer_addr);
                    end = (std::min)(end, buffer_end);
                    if (start >= end) {
                        return;
                    }
                    const u64 new_offset = start - buffer_addr;
                    const u64 new_size = end - start;
                    copies.push_back(BufferCopy{
                        .src_offset = new_offset,
                        .dst_offset = total_size_bytes,
                        .size = new_size,
                    });
                    // Align up to avoid cache conflicts
                    constexpr u64 align = 64ULL;
                    constexpr u64 mask = ~(align - 1ULL);
                    total_size_bytes += (new_size + align - 1) & mask;
                    largest_copy = (std::max)(largest_copy, new_size);
                };

                gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
                ClearDownload(device_addr_out, range_size);
                gpu_modified_ranges.Subtract(device_addr_out, range_size);
            });
    }
    if (total_size_bytes == 0) {
        return;
    }
    const Common::RangeSet<DAddr> cpu_written = PendingCpuWriteRanges();
    if constexpr (requires { runtime.CanReadBuffersDirectly(); buffer.HostView(); }) {
        // An integrated GPU's buffers are in the memory the host reads: wait for the GPU to
        // complete the last command that wrote this buffer (often done already) and read it,
        // instead of copying it out and waiting for all the GPU's work. Mario Kart World's CPU
        // reads GPU-written data all the time, each such wait 17% of the GPU back thread.
        const u64 tick = buffer.GpuWriteTick();
        if (tick != 0 && runtime.CanReadBuffersDirectly()) {
            const std::span<const u8> host = buffer.HostView();
            if (!host.empty()) {
                // EDEN_NX2_GPU_DIRTY_GEN: only for the last write to the pages read (every write
                // to the buffer notes its pages), not for later ones elsewhere in the buffer.
                u64 wait_tick = tick;
                if (Tegra::Nx2GpuDirtyGen()) {
                    u64 pages_tick = 0;
                    for (const BufferCopy& copy : copies) {
                        pages_tick = (std::max)(pages_tick,
                                                Tegra::Nx2LastGpuWriteTick(
                                                    buffer.CpuAddr() + copy.src_offset, copy.size));
                    }
                    if (pages_tick != 0 && pages_tick < tick) {
                        wait_tick = pages_tick;
                        if (Tegra::Nx2StratoStats()) {
                            VideoCommon::Nx2Strato::NoteGenDownload(
                                runtime.TickComplete(pages_tick) && !runtime.IsFree(tick));
                        }
                    }
                }
                runtime.WaitTick(wait_tick);
                buffer.InvalidateHostView();
                for (const BufferCopy& copy : copies) {
                    WriteDownloadedMemory(cpu_written, buffer.CpuAddr() + copy.src_offset,
                                          host.data() + copy.src_offset, copy.size);
                }
                return;
            }
        }
    }

    if constexpr (USE_MEMORY_MAPS) {
        const bool batched = !IS_OPENGL && InDownloadBatch();
        auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes, batched);
        const u8* const mapped_memory = download_staging.mapped_span.data();
        const std::span<BufferCopy> copies_span(copies.data(), copies.data() + copies.size());
        for (BufferCopy& copy : copies) {
            // Modify copies to have the staging offset in mind
            copy.dst_offset += download_staging.offset;
            buffer.MarkUsage(copy.src_offset, copy.size);
        }
        runtime.CopyBuffer(download_staging.buffer, buffer, copies_span, true);
        if constexpr (!IS_OPENGL) {
            if (batched) {
                batched_downloads.push_back({download_staging, buffer.CpuAddr(),
                                             {copies.begin(), copies.end()}});
                return;
            }
        }
        runtime.Finish();
        for (const BufferCopy& copy : copies) {
            const DAddr copy_device_addr = buffer.CpuAddr() + copy.src_offset;
            // Undo the modified offset
            const u64 dst_offset = copy.dst_offset - download_staging.offset;
            const u8* copy_mapped_memory = mapped_memory + dst_offset;
            WriteDownloadedMemory(cpu_written, copy_device_addr, copy_mapped_memory, copy.size);
        }
    } else {
        const std::span<u8> immediate_buffer = ImmediateBuffer(largest_copy);
        for (const BufferCopy& copy : copies) {
            buffer.ImmediateDownload(copy.src_offset, immediate_buffer.subspan(0, copy.size));
            const DAddr copy_device_addr = buffer.CpuAddr() + copy.src_offset;
            WriteDownloadedMemory(cpu_written, copy_device_addr, immediate_buffer.data(), copy.size);
        }
    }
}

template <class P>
void BufferCache<P>::DeleteBuffer(BufferId buffer_id, bool do_not_mark) {
    if (buffer_id == inline_buffer_id) {
        // The garbage collector can delete the inline index buffer; recreate it on next use.
        inline_buffer_id = NULL_BUFFER_ID;
    }
    if constexpr (requires { runtime.OnBufferDeleted(slot_buffers[buffer_id]); }) {
        runtime.OnBufferDeleted(slot_buffers[buffer_id]);
    }
    read_trapped_ranges.Subtract(slot_buffers[buffer_id].CpuAddr(),
                                 slot_buffers[buffer_id].SizeBytes());
    cpu_read_ranges.Subtract(slot_buffers[buffer_id].CpuAddr(), slot_buffers[buffer_id].SizeBytes());
    bool dirty_index{false};
    boost::container::small_vector<u64, NUM_VERTEX_BUFFERS> dirty_vertex_buffers;
    const auto scalar_replace = [buffer_id](Binding& binding) {
        if (binding.buffer_id == buffer_id) {
            binding.buffer_id = BufferId{};
        }
    };
    const auto replace = [scalar_replace](std::span<Binding> bindings) {
        std::ranges::for_each(bindings, scalar_replace);
    };

    if (channel_state->index_buffer.buffer_id == buffer_id) {
        channel_state->index_buffer.buffer_id = BufferId{};
        dirty_index = true;
    }
    // Multi-range storage segments too: the update pass resolves them again.
    for (auto* const pool : {&graphics_segments, &compute_segments}) {
        for (MultiRangeSegment& segment : *pool) {
            if (segment.buffer_id == buffer_id) {
                segment.buffer_id = BufferId{};
            }
        }
    }

    for (u32 index = 0; index < channel_state->vertex_buffers.size(); index++) {
        auto& binding = channel_state->vertex_buffers[index];
        if (binding.buffer_id == buffer_id) {
            binding.buffer_id = BufferId{};
            UpdateVertexBufferSlot(index, binding);
            dirty_vertex_buffers.push_back(index);
        }
    }
    std::ranges::for_each(channel_state->uniform_buffers, replace);
    std::ranges::for_each(channel_state->storage_buffers, replace);
    replace(channel_state->transform_feedback_buffers);
    replace(channel_state->compute_uniform_buffers);
    replace(channel_state->compute_storage_buffers);

    synced_ranges.fill({});
    // Mark the whole buffer as CPU written to stop tracking CPU writes
    if (!do_not_mark) {
        Buffer& buffer = slot_buffers[buffer_id];
        memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddr(), buffer.SizeBytes());
    }

    Unregister(buffer_id);

#ifdef YUZU_LEGACY
    if (!do_not_mark || !immediately_free)
#endif
        delayed_destruction_ring.Push(std::move(slot_buffers[buffer_id]));

    slot_buffers.erase(buffer_id);

    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        channel_state->dirty_uniform_buffers.fill(~u32{0});
        channel_state->uniform_buffer_binding_sizes.fill({});
    }

    auto& flags = maxwell3d->dirty.flags;
    if (dirty_index) {
        flags[Dirty::IndexBuffer] = true;
    }

    if (dirty_vertex_buffers.size() > 0) {
        flags[Dirty::VertexBuffers] = true;
        for (auto index : dirty_vertex_buffers) {
            flags[Dirty::VertexBuffer0 + index] = true;
        }
    }
    channel_state->has_deleted_buffers = true;
}

template <class P>
Binding BufferCache<P>::StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index,
                                             bool is_written, bool native_pointer,
                                             bool table_pointer) const {
    // A pointer read from a table in memory is the base of an object pool, read anywhere in it
    // (Ocarina of Time's grass instances lie 12-18 MiB past theirs): a read-only one is bound to
    // the end of its mapping, up to EDEN_NX2_TABLE_SSBO_MIB MiB (default 64; 0 turns it off).
    // The 8 MiB cap made those reads return 0 and culled every blade. Written pools keep the cap:
    // a write marks the whole binding GPU-modified.
    static const u32 table_cap = [] {
        const char* const value = std::getenv("EDEN_NX2_TABLE_SSBO_MIB");
        const unsigned long mib = value != nullptr ? std::strtoul(value, nullptr, 10) : 64UL;
        return static_cast<u32>((std::min)(mib, 1024UL) * 1_MiB);
    }();
    const GPUVAddr gpu_addr = gpu_memory->Read<u64>(ssbo_addr);

    if (gpu_addr == 0) {
        return NULL_BINDING;
    }

    // The binding of a pointer depends only on the address space's page tables and kind map, the
    // size word after the pointer, the alignment and the access: keep it until the page tables
    // change. Each miss takes the kind map's lock (GetMemoryLayoutSize) and walks the page tables
    // for every storage buffer of every draw and dispatch. EDEN_NX2_SSBO_MEMO=0 turns it off.
    static const bool memoize = [] {
        const char* value = std::getenv("EDEN_NX2_SSBO_MEMO");
        return value == nullptr || value[0] != '0';
    }();
    const bool table_sized = table_pointer && !is_written && table_cap != 0;
    const u64 next_qword =
        native_pointer || table_sized ? 0 : gpu_memory->Read<u64>(ssbo_addr + 8);
    const u32 alignment = runtime.GetStorageBufferAlignment();
    const u32 flags =
        (is_written ? 1U : 0U) | (native_pointer ? 2U : 0U) | (table_pointer ? 4U : 0U);
    const size_t owner = gpu_memory->GetID() + 1;
    // Before anything is read from the page tables: a change after it bumps the serial, so an
    // answer built from tables in flux is never found again.
    const u64 serial = gpu_memory->MapSerial();
    auto& set = storage_binding_memo[((gpu_addr ^ next_qword ^ flags) * 0x9E3779B97F4A7C15ULL) >>
                                     (64 - StorageBindingMemoSetBits)];
    if (memoize) {
        for (size_t way = 0; way < set.size(); ++way) {
            const StorageBindingMemo& cached = set[way];
            if (cached.owner == owner && cached.gpu_addr == gpu_addr &&
                cached.next_qword == next_qword && cached.serial == serial &&
                cached.alignment == alignment && cached.flags == flags) {
                const Binding hit = cached.binding;
                std::rotate(set.begin(), set.begin() + way, set.begin() + way + 1);
                return hit;
            }
        }
    }

    const Binding binding = [&]() -> Binding {
        u32 size = [&]() {
            const u32 memory_layout_size =
                static_cast<u32>(gpu_memory->GetMemoryLayoutSize(gpu_addr));
            if (table_sized) {
                return (std::min)(memory_layout_size, table_cap);
            }
            // A native (Switch 2) compute program's pointer is bare: the word after it is another
            // parameter (a dimension, stride or texture handle), not an NVN buffer size. Taking it
            // as one bound Fortune's Weave's DLSS input features to 266 bytes and dropped the
            // rest. Only DLSS passes size from the mapping: doing it for every native dispatch
            // binds up to 8 MiB per buffer and slows Hyrule Warriors' fights to a few fps, then
            // hangs them.
            if (native_pointer) {
                return (std::min)(memory_layout_size, static_cast<u32>(8_MiB));
            }
            const u32 packed_size = static_cast<u32>(next_qword);
            const bool next_qword_is_size = static_cast<u32>(next_qword >> 32) == 0 &&
                                            packed_size != 0 &&
                                            packed_size <= memory_layout_size;
            if (next_qword_is_size) {
                return packed_size;
            }
            return (std::min)(memory_layout_size, static_cast<u32>(8_MiB));
        }();
        if ((native_pointer || table_pointer) && size != 0) {
            // Stop where the mapping or its device range does: the binding is one device range,
            // and a written one isn't split into separate ranges without sparse sources.
            size = static_cast<u32>(gpu_memory->MaxContinuousRange(gpu_addr, size));
        }

        // Alignment only applies to the offset of the buffer
        const GPUVAddr aligned_gpu_addr = Common::AlignDown(gpu_addr, alignment);
        const u32 aligned_size = static_cast<u32>(gpu_addr - aligned_gpu_addr) + size;

        const std::optional<DAddr> aligned_device_addr =
            gpu_memory->GpuToCpuAddress(aligned_gpu_addr);
        if (!aligned_device_addr || size == 0) {
            LOG_DEBUG(HW_GPU, "Failed to find storage buffer for cbuf index {}", cbuf_index);
            return NULL_BINDING;
        }
        const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        ASSERT_MSG(device_addr, "Unaligned storage buffer address not found for cbuf index {}",
                   cbuf_index);
        // The end address used for size calculation does not need to be aligned
        const DAddr cpu_end = Common::AlignUp(*device_addr + size, Core::DEVICE_PAGESIZE);

        u32 binding_size = static_cast<u32>(cpu_end - *aligned_device_addr);
        if (is_written) {
            binding_size = aligned_size;
        }
        return Binding{
            .device_addr = *aligned_device_addr,
            .gpu_addr = aligned_gpu_addr,
            .size = binding_size,
            .buffer_id = BufferId{},
        };
    }();
    if (memoize) {
        std::rotate(set.begin(), set.end() - 1, set.end());
        set[0] = StorageBindingMemo{owner, gpu_addr, next_qword, serial, alignment, flags, binding};
    }
    return binding;
}

template <class P>
TextureBufferBinding BufferCache<P>::GetTextureBufferBinding(GPUVAddr gpu_addr, u32 size,
                                                             PixelFormat format) {
    const std::optional<DAddr> device_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    TextureBufferBinding binding;
    if (!device_addr || size == 0) {
        binding.device_addr = 0;
        binding.size = 0;
        binding.buffer_id = NULL_BUFFER_ID;
        binding.format = PixelFormat::Invalid;
    } else {
        binding.device_addr = *device_addr;
        binding.size = size;
        binding.buffer_id = BufferId{};
        binding.format = format;
    }
    return binding;
}

template <class P>
std::span<const u8> BufferCache<P>::ImmediateBufferWithData(DAddr device_addr, size_t size) {
    u8* const base_pointer = device_memory.GetPointer<u8>(device_addr);
    if (IsRangeGranular(device_addr, size) ||
        base_pointer + size == device_memory.GetPointer<u8>(device_addr + size)) {
        return std::span(base_pointer, size);
    } else {
        const std::span<u8> span = ImmediateBuffer(size);
        device_memory.ReadBlockUnsafe(device_addr, span.data(), size);
        return span;
    }
}

template <class P>
std::span<u8> BufferCache<P>::ImmediateBuffer(size_t wanted_capacity) {
    immediate_buffer_alloc.resize_destructive(wanted_capacity);
    return std::span<u8>(immediate_buffer_alloc.data(), wanted_capacity);
}

template <class P>
bool BufferCache<P>::HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept {
    return ((channel_state->fast_bound_uniform_buffers[stage] >> binding_index) & 1u) != 0;
}

template <class P>
std::pair<typename BufferCache<P>::Buffer*, u32> BufferCache<P>::GetDrawIndirectCount() {
    auto& buffer = slot_buffers[channel_state->count_buffer_binding.buffer_id];
    return std::make_pair(&buffer, buffer.Offset(channel_state->count_buffer_binding.device_addr));
}

template <class P>
std::pair<typename BufferCache<P>::Buffer*, u32> BufferCache<P>::GetDrawIndirectBuffer() {
    auto& buffer = slot_buffers[channel_state->indirect_buffer_binding.buffer_id];
    return std::make_pair(&buffer,
                          buffer.Offset(channel_state->indirect_buffer_binding.device_addr));
}

} // namespace VideoCommon
