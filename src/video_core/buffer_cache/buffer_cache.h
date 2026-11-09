// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fmt/ranges.h>

#include "common/range_sets.inc"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/uniform_stream_reuse.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
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
    immediately_free =
        (Settings::values.vram_usage_mode.GetValue() == Settings::VramUsageMode::Aggressive);
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
        if (!aggressive_gc &&
            gpu_modified_ranges.Intersects(buffer.CpuAddr(), buffer.SizeBytes())) {
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
    if (native_indirect_uniform_shadow.size() > 256) {
        native_indirect_uniform_shadow.clear();
        native_indirect_uniform_cache.fill({~DAddr{0}, nullptr});
        native_indirect_uniform_last_bank = ~DAddr{0};
        native_indirect_uniform_last_ptr = nullptr;
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
                 fmt::join(largest.begin(), largest.begin() + std::min<size_t>(6, largest.size()),
                           ", "));
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
    if (frame_tick % 60 == 0 && Nx2Perf::On(Nx2Perf::Opt::PreflushAging)) {
        Nx2AgePreflushBlocks();
    }
    delayed_destruction_ring.Tick();

    if (!async_buffers_death_ring.empty()) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
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
    const DAddr page_begin = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
    const DAddr page_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
    // [device_addr, device_addr + size) lies within [page_begin, page_end): one lookup answers
    // whether the written range or its surrounding 4 KiB page holds any GPU-modified bytes.
    if (gpu_modified_ranges.Intersects(page_begin, page_end - page_begin)) [[unlikely]] {
        if (IsRegionGpuModified(device_addr, size)) {
            ClearDownload(device_addr, size);
            gpu_modified_ranges.Subtract(device_addr, size);
        }
        // The tracker marks whole pages CPU-modified, which drops their GPU-modified state:
        // GPU-written bytes elsewhere on these pages would never be downloaded, and the page's next
        // upload would send the stale guest copy over them. Hyrule Warriors: Age of Imprisonment
        // writes small structures next to the draw arguments its compute dispatches fill in; the
        // draws then read leftover bytes as vertex counts. Download those bytes first (the CPU's
        // own were subtracted above, so they aren't overwritten).
        // Only the scheduler's owner may download (it records and submits): unmaps and other writes
        // reach this from CPU and service threads too, and submitting there raced the GPU thread's
        // own submissions (crashes in Scheduler::SubmitExecution). Those keep the old behaviour;
        // guest CPU writes come through TryCacheInvalidation, which sends such pages to the GPU
        // thread.
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
    }
    memory_tracker.MarkRegionAsCpuModified(device_addr, size);
    MarkVertexBuffersStale(device_addr, size);
    last_synced_count = 0;
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
    // Preflushable data is downloaded at fences, but only where it is marked (QueueGpuWrite): the
    // rest of the range still needs this flush.
    if (memory_tracker.IsRegionPreflushable(device_addr, size) &&
        !HasGpuWritesNotDownloadedAtFence(device_addr_start_aligned,
                                          device_addr_end_aligned - device_addr_start_aligned)) {
        area->preemtive = true;
        // The core keeps this area: a later GPU write to its unmarked pages must arm them again.
        read_trapped_ranges.Subtract(device_addr_start_aligned,
                                     device_addr_end_aligned - device_addr_start_aligned);
        if (Nx2Perf::On(Nx2Perf::Opt::PreflushAging)) {
            Nx2StampPreflushBlocks(device_addr_start_aligned, device_addr_end_aligned, false);
        }
        return area;
    };
    area->preemtive = !IsRegionGpuModified(device_addr_start_aligned,
                                           device_addr_end_aligned - device_addr_start_aligned);
    // A preflushable mark persists and downloads later GPU writes at every fence. Trapping every
    // read instead stalled Ocarina of Time ~55 times a second; EDEN_NX2_PREFLUSH=0 keeps read
    // traps.
    static const std::optional<bool> forced_preflush = []() -> std::optional<bool> {
        const char* value = std::getenv("EDEN_NX2_PREFLUSH");
        if (value != nullptr) {
            return value[0] != '0';
        }
        return std::nullopt;
    }();
    const bool preflush =
        forced_preflush.value_or(true);
    if (!area->preemtive) {
        if (preflush) {
            memory_tracker.MarkRegionAsPreflushable(
                device_addr_start_aligned, device_addr_end_aligned - device_addr_start_aligned);
            Nx2StampPreflushBlocks(device_addr_start_aligned, device_addr_end_aligned, true);
            // The core consumed this trap too: ArmReadTraps arms only what the tracker counts as
            // unarmed, so once the mark ages out the next GPU write must arm these pages again.
            read_trapped_ranges.Subtract(
                device_addr_start_aligned, device_addr_end_aligned - device_addr_start_aligned);
        } else {
            if constexpr (USE_MEMORY_MAPS) {
                cpu_read_ranges.Add(area->start_address, area->end_address - area->start_address);
                download_cpu_read_ranges = true;
            }
            // The core consumed this trap before GetFlushArea. Forget it so the next GPU write
            // really rearms the core instead of being skipped by the deduplication cache.
            read_trapped_ranges.Subtract(
                device_addr_start_aligned, device_addr_end_aligned - device_addr_start_aligned);
        }
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
    if constexpr (USE_MEMORY_MAPS) {
        if (download_cpu_read_ranges) {
            download_cpu_read_ranges = false;
            boost::container::small_vector<std::pair<DAddr, u64>, 16> ranges{{device_addr, size}};
            cpu_read_ranges.ForEach([&](DAddr lower, DAddr upper) {
                gpu_modified_ranges.ForEachInRange(lower, upper - lower,
                                                   [&](DAddr start, DAddr end) {
                                                       ranges.emplace_back(start, end - start);
                                                       const DAddr page_begin =
                                                           Common::AlignDown(start, Core::DEVICE_PAGESIZE);
                                                       const DAddr page_end =
                                                           Common::AlignUp(end, Core::DEVICE_PAGESIZE);
                                                       read_trapped_ranges.Subtract(
                                                           page_begin, page_end - page_begin);
                                                   });
            });
            DownloadRangesMemory(ranges);
            return;
        }
    }
    ForEachBufferInRange(device_addr, size, [&](BufferId, Buffer& buffer) {
        const DAddr start = (std::max)(device_addr, buffer.CpuAddr());
        const DAddr end = (std::min)(device_addr + size, buffer.CpuAddr() + buffer.SizeBytes());
        if (start < end) {
            DownloadBufferMemory(buffer, start, end - start);
        }
    });
}

template <class P>
void BufferCache<P>::DownloadRangesMemory(std::span<const std::pair<DAddr, u64>> ranges) {
    struct Download {
        BufferId buffer_id;
        BufferCopy copy;
    };
    boost::container::small_vector<Download, 16> downloads;
    u64 total_size_bytes = 0;
    for (const auto& [range_addr, range_size] : ranges) {
        ForEachBufferInRange(range_addr, range_size, [&](BufferId buffer_id, Buffer& buffer) {
            const DAddr buffer_addr = buffer.CpuAddr();
            const DAddr start = (std::max)(range_addr, buffer_addr);
            const DAddr end = (std::min)(range_addr + range_size, buffer_addr + buffer.SizeBytes());
            if (start >= end) {
                return;
            }
            memory_tracker.ForEachDownloadRangeAndClear(
                start, end - start, [&](u64 device_addr_out, u64 out_size) {
                    gpu_modified_ranges.ForEachInRange(
                        device_addr_out, out_size, [&](DAddr lower, DAddr upper) {
                            downloads.push_back({buffer_id, BufferCopy{
                                                                .src_offset = lower - buffer_addr,
                                                                .dst_offset = total_size_bytes,
                                                                .size = upper - lower,
                                                            }});
                            total_size_bytes += Common::AlignUp(upper - lower, 64);
                        });
                    ClearDownload(device_addr_out, out_size);
                    gpu_modified_ranges.Subtract(device_addr_out, out_size);
                });
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
void BufferCache<P>::ClearDownload(DAddr device_addr, u64 size) {
    memory_tracker.BumpGpuEpoch();
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
        QueueGpuWrite(new_base_address, size);
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
    runtime.CopyBuffer(dest_buffer, src_buffer, copies, true);
    if (has_new_downloads) {
        memory_tracker.MarkRegionAsGpuModified(*cpu_dest_address, amount);
        const bool should_sync =
            Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
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
    runtime.ClearBuffer(dest_buffer, offset, size, value);
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
    ++native_indirect_uniform_draw_ticket;
    last_synced_count = 0;
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
    if (stage == 0) {
        ++native_indirect_uniform_draw_ticket;
        last_synced_count = 0;
    }
    if (channel_state->enabled_uniform_buffer_masks[stage] != 0) {
        BindHostGraphicsUniformBuffers(stage);
    }
    if (channel_state->enabled_storage_buffers[stage] != 0) {
        BindHostGraphicsStorageBuffers(stage);
    }
    if (channel_state->enabled_texture_buffers[stage] != 0) {
        BindHostGraphicsTextureBuffers(stage);
    }
}

template <class P>
void BufferCache<P>::BindHostComputeBuffers() {
    ++native_indirect_uniform_draw_ticket;
    last_synced_count = 0;
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
bool BufferCache<P>::BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index,
                                               u32 cbuf_offset, bool is_written,
                                               u32 indirect_offset) {
    if (stage >= NUM_STAGES || ssbo_index >= NUM_STORAGE_BUFFERS) {
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
                LOG_WARNING(HW_GPU, "Skipping graphics storage buffer {} due to driver limit {}",
                            ssbo_index, max_bindings);
                return false;
            }
        }
    }
    channel_state->enabled_storage_buffers[stage] |= u64{1} << ssbo_index;
    channel_state->written_storage_buffers[stage] |= u64{is_written ? 1U : 0U} << ssbo_index;
    if constexpr (requires { runtime.ShouldLimitDynamicStorageBuffers(); }) {
        if (runtime.ShouldLimitDynamicStorageBuffers() && !already_enabled) {
            ++channel_state->total_graphics_storage_buffers;
        }
    }

    const auto& cbufs = maxwell3d->state.shader_stages[stage];
    GPUVAddr ssbo_addr = cbufs.const_buffers[cbuf_index].address + cbuf_offset;
    if (indirect_offset != ~0U) {
        // The pointer is an entry of a table the constant buffer points to.
        const GPUVAddr holder = gpu_memory->Read<u64>(ssbo_addr);
        if (holder == 0) {
            channel_state->storage_buffers[stage][ssbo_index] = NULL_BINDING;
            return false;
        }
        ssbo_addr = holder + indirect_offset;
    }
    Binding& slot = channel_state->storage_buffers[stage][ssbo_index];
    Binding next = StorageBufferBinding(ssbo_addr, cbuf_index, is_written,
                                        maxwell3d->GetNativeMacroEngine().native_turing,
                                        indirect_offset != ~0U);
    if (slot.buffer_id && slot.buffer_id != NULL_BUFFER_ID &&
        slot.device_addr == next.device_addr && slot.gpu_addr == next.gpu_addr &&
        slot.size == next.size) {
        next.buffer_id = slot.buffer_id;
    }
    slot = next;
    return (slot.buffer_id != NULL_BUFFER_ID);
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
                LOG_WARNING(HW_GPU, "Skipping compute storage buffer {} due to driver limit {}",
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
    Binding& slot = channel_state->compute_storage_buffers[ssbo_index];
    Binding next = StorageBufferBinding(ssbo_addr, cbuf_index, is_written,
                                        kepler_compute->IsDlssDispatch(), indirect_offset != ~0U);
    if (slot.buffer_id && slot.buffer_id != NULL_BUFFER_ID &&
        slot.device_addr == next.device_addr && slot.gpu_addr == next.gpu_addr &&
        slot.size == next.size) {
        next.buffer_id = slot.buffer_id;
    }
    slot = next;
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

    Nx2Downloads downloads;
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
    // One copy command per buffer, with nearby ranges merged: see Nx2BatchDownloads.
    Nx2Downloads batched;
    u64 batched_size_bytes = 0;
    if (Nx2Perf::On(Nx2Perf::Opt::DownloadBatch)) {
        batched_size_bytes = Nx2BatchDownloads(downloads, batched);
        if (Nx2Perf::Verify() && !Nx2VerifyDownloadBatch(downloads, batched)) {
            batched.clear();
        }
    }
    const bool use_batched = !batched.empty();
    auto download_staging =
        runtime.DownloadStagingBuffer(use_batched ? batched_size_bytes : total_size_bytes, true);
    boost::container::small_vector<BufferCopy, 4> normalized_copies;
    Common::RangeSet<DAddr> download_ranges;
    runtime.PreCopyBarrier();
    for (auto& [copy, buffer_id] : downloads) {
        Buffer& buffer = slot_buffers[buffer_id];
        const DAddr orig_device_addr = static_cast<DAddr>(buffer.CpuAddr()) + copy.src_offset;
        if (Nx2Profile::Enabled()) {
            Nx2CountDownloadAges(orig_device_addr, copy.size);
            u64 on_dma_pages = 0;
            const auto& pages = Nx2Profile::DmaReadPages();
            for (DAddr page = orig_device_addr >> 12;
                 page <= (orig_device_addr + copy.size - 1) >> 12; ++page) {
                if (pages.contains(page)) {
                    const DAddr begin = (std::max)(orig_device_addr, page << 12);
                    const DAddr end = (std::min)(orig_device_addr + copy.size, (page + 1) << 12);
                    on_dma_pages += end - begin;
                }
            }
            Nx2Profile::Add(Nx2Profile::Counter::DownloadBytesOnDmaPages, on_dma_pages);
        }
        // Exact ranges even when batched: gap bytes reach staging, never guest memory.
        download_ranges.Add(orig_device_addr, copy.size);
        buffer.MarkUsage(copy.src_offset, copy.size);
        if (use_batched) {
            continue;
        }
        copy.dst_offset += download_staging.offset;
        const std::array copies{copy};
        BufferCopy second_copy{copy};
        second_copy.src_offset = static_cast<size_t>(orig_device_addr);
        runtime.CopyBuffer(download_staging.buffer, buffer, copies, false);
        normalized_copies.push_back(second_copy);
    }
    if (use_batched) {
        boost::container::small_vector<BufferCopy, 16> copies;
        for (size_t i = 0; i < batched.size();) {
            const BufferId buffer_id = batched[i].second;
            Buffer& buffer = slot_buffers[buffer_id];
            copies.clear();
            for (; i < batched.size() && batched[i].second == buffer_id; ++i) {
                BufferCopy copy = batched[i].first;
                copy.dst_offset += download_staging.offset;
                copies.push_back(copy);
                BufferCopy second_copy{copy};
                second_copy.src_offset = static_cast<size_t>(buffer.CpuAddr()) + copy.src_offset;
                normalized_copies.push_back(second_copy);
            }
            runtime.CopyBuffer(download_staging.buffer, buffer,
                               std::span<const BufferCopy>(copies.data(), copies.size()), false);
        }
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
u64 BufferCache<P>::Nx2BatchDownloads(const Nx2Downloads& downloads, Nx2Downloads& batched) {
    // Ocarina downloaded ~13 ranges per fence, each its own recorded copy command. Merged ranges
    // copy their gap (at most 4 KiB) to staging too, but PopAsyncBuffers writes back only
    // pending_download_ranges, which keeps the exact ranges.
    constexpr u64 max_gap = 4_KiB;
    batched.assign(downloads.begin(), downloads.end());
    std::sort(batched.begin(), batched.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.index != rhs.second.index) {
            return lhs.second.index < rhs.second.index;
        }
        return lhs.first.src_offset < rhs.first.src_offset;
    });
    size_t last = 0;
    for (size_t i = 1; i < batched.size(); ++i) {
        BufferCopy& merged = batched[last].first;
        const BufferCopy& next = batched[i].first;
        const u64 merged_end = merged.src_offset + merged.size;
        if (batched[i].second == batched[last].second && next.src_offset <= merged_end + max_gap) {
            merged.size = (std::max)(merged_end, next.src_offset + next.size) - merged.src_offset;
            continue;
        }
        batched[++last] = batched[i];
    }
    batched.erase(batched.begin() + last + 1, batched.end());
    u64 total_size_bytes = 0;
    for (auto& [copy, buffer_id] : batched) {
        copy.dst_offset = total_size_bytes;
        total_size_bytes += Common::AlignUp(copy.size, 64);
    }
    return total_size_bytes;
}

template <class P>
bool BufferCache<P>::Nx2VerifyDownloadBatch(const Nx2Downloads& downloads,
                                            const Nx2Downloads& batched) {
    static std::atomic<u64> mismatches{};
    bool ok = true;
    const auto report = [&](std::string_view what, BufferId buffer_id, u64 offset, u64 size) {
        ok = false;
        const u64 count = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf download_batch: {} (buffer {}, offset {:#x}, size {:#x}; {} copies, "
                      "{} batched; mismatch {})",
                      what, buffer_id.index, offset, size, downloads.size(), batched.size(),
                      count);
        }
    };
    // Every unbatched copy lies inside a batched region of its buffer.
    for (const auto& [copy, buffer_id] : downloads) {
        const bool covered = std::ranges::any_of(batched, [&](const auto& region) {
            return region.second == buffer_id && region.first.src_offset <= copy.src_offset &&
                   copy.src_offset + copy.size <= region.first.src_offset + region.first.size;
        });
        if (!covered) {
            report("copy not covered", buffer_id, copy.src_offset, copy.size);
        }
    }
    // PopAsyncBuffers writes back the exact ranges inside each batched region: the same bytes as
    // the unbatched copies, each once, and nothing from the gaps.
    Common::RangeSet<DAddr> exact;
    u64 exact_bytes = 0;
    for (const auto& [copy, buffer_id] : downloads) {
        exact.Add(slot_buffers[buffer_id].CpuAddr() + copy.src_offset, copy.size);
        exact_bytes += copy.size;
    }
    u64 written_bytes = 0;
    u64 staging_end = 0;
    for (const auto& [region, buffer_id] : batched) {
        const Buffer& buffer = slot_buffers[buffer_id];
        if (region.src_offset + region.size > buffer.SizeBytes() ||
            region.dst_offset < staging_end) {
            report("region outside its buffer or overlapping in staging", buffer_id,
                   region.src_offset, region.size);
        }
        staging_end = region.dst_offset + region.size;
        exact.ForEachInRange(buffer.CpuAddr() + region.src_offset, region.size,
                             [&](DAddr start, DAddr end) { written_bytes += end - start; });
    }
    if (written_bytes != exact_bytes) {
        report("bytes written back differ", BufferId{}, written_bytes, exact_bytes);
    }
    return ok;
}

template <class P>
void BufferCache<P>::Nx2CountDownloadAges(DAddr device_addr, u64 size) {
    using Nx2Profile::Counter;
    constexpr u64 block_size = u64{1} << NX2_PREFLUSH_BLOCK_BITS;
    const DAddr end = device_addr + size;
    for (u64 block = device_addr >> NX2_PREFLUSH_BLOCK_BITS;
         block <= (end - 1) >> NX2_PREFLUSH_BLOCK_BITS; ++block) {
        const DAddr begin = (std::max)(device_addr, block * block_size);
        const DAddr block_end = (std::min)(end, (block + 1) * block_size);
        Counter counter = Counter::DownloadAgeNone;
        if (const auto it = nx2_preflush_blocks.find(block); it != nx2_preflush_blocks.end()) {
            const u64 age = frame_tick - it->second.last_read_frame;
            counter = age < 60     ? Counter::DownloadAge60
                      : age < 120  ? Counter::DownloadAge120
                      : age < 600  ? Counter::DownloadAge600
                      : age < 3600 ? Counter::DownloadAge3600
                                   : Counter::DownloadAgeOlder;
        }
        Nx2Profile::Add(counter, block_end - begin);
    }
}

template <class P>
void BufferCache<P>::Nx2StampPreflushBlocks(DAddr begin, DAddr end, bool marked) {
    const bool aging = Nx2Perf::On(Nx2Perf::Opt::PreflushAging);
    for (u64 block = begin >> NX2_PREFLUSH_BLOCK_BITS;
         block <= (end - 1) >> NX2_PREFLUSH_BLOCK_BITS; ++block) {
        if (!marked) {
            // A read of a marked range may span blocks without marks: stamp only marked ones.
            if (const auto it = nx2_preflush_blocks.find(block);
                it != nx2_preflush_blocks.end() && !it->second.dropped) {
                it->second.last_read_frame = frame_tick;
            }
            continue;
        }
        const auto [it, inserted] =
            nx2_preflush_blocks.try_emplace(block, Nx2PreflushBlock{.last_read_frame = frame_tick});
        if (inserted || !aging) {
            continue;
        }
        Nx2PreflushBlock& entry = it->second;
        entry.last_read_frame = frame_tick;
        if (!entry.dropped) {
            continue;
        }
        // Dropped too early: the read flushed (stalled). Keep the mark twice as long next time.
        entry.dropped = false;
        entry.limit_frames = (std::min)(entry.limit_frames * 2, NX2_PREFLUSH_MAX_LIMIT);
        entry.limit_frame = frame_tick;
        ++nx2_preflush_remarks;
        if (Nx2Perf::Verify()) {
            static u64 reports{};
            if (++reports <= 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf preflush_aging: read of dropped block {:#x} stalled (read "
                          "{:#x}-{:#x}, new limit {} frames; {} stalls in {} drops)",
                          block << NX2_PREFLUSH_BLOCK_BITS, begin, end, entry.limit_frames,
                          nx2_preflush_remarks, nx2_preflush_drops);
            }
        }
    }
}

template <class P>
void BufferCache<P>::Nx2AgePreflushBlocks() {
    // Invariant: GPU data not yet in guest memory (gpu_modified_ranges) is either in the download
    // pipeline with its mark (marked when QueueGpuWrite saw the write) or read-trapped (unmarked
    // at the write: QueueGpuWrite armed it; dropped here before PopAsyncBuffers wrote it back:
    // armed below). A read of a dropped block then takes GetFlushArea's flush path, which waits
    // for the GPU's data, and marks the block again.
    constexpr u64 block_size = u64{1} << NX2_PREFLUSH_BLOCK_BITS;
    size_t marked = 0;
    for (auto it = nx2_preflush_blocks.begin(); it != nx2_preflush_blocks.end();) {
        Nx2PreflushBlock& entry = it->second;
        if (entry.dropped) {
            // The limit halves per 3600 frames unread; back at the minimum the block is forgotten.
            if (frame_tick - entry.limit_frame >= NX2_PREFLUSH_MAX_LIMIT) {
                if (entry.limit_frames <= NX2_PREFLUSH_MIN_LIMIT) {
                    it = nx2_preflush_blocks.erase(it);
                    continue;
                }
                entry.limit_frames = (std::max)(entry.limit_frames / 2, NX2_PREFLUSH_MIN_LIMIT);
                entry.limit_frame = frame_tick;
            }
            ++it;
            continue;
        }
        if (frame_tick - entry.last_read_frame <= entry.limit_frames) {
            ++marked;
            ++it;
            continue;
        }
        const DAddr begin = static_cast<DAddr>(it->first << NX2_PREFLUSH_BLOCK_BITS);
        memory_tracker.UnmarkRegionAsPreflushable(begin, block_size);
        gpu_modified_ranges.ForEachInRange(begin, block_size, [&](DAddr start, DAddr end) {
            ArmReadTraps(start, end - start);
        });
        entry.dropped = true;
        entry.limit_frame = frame_tick;
        ++nx2_preflush_drops;
        ++it;
    }
    if (frame_tick % 3600 == 0) {
        LOG_INFO(HW_GPU,
                 "Nx2Perf preflush_aging: {} blocks marked, {} dropped; {} drops and {} stalled "
                 "reads of dropped blocks since start",
                 marked, nx2_preflush_blocks.size() - marked, nx2_preflush_drops,
                 nx2_preflush_remarks);
    }
}

template <class P>
void BufferCache<P>::PopAsyncFlushes() {
    PopAsyncBuffers();
}

template <class P>
void BufferCache<P>::ClaimCpuWrites(std::span<const std::pair<DAddr, u64>> ranges) {
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
    if (pending_cpu_writes) {
        pending_cpu_writes(
            [&ranges](DAddr device_addr, size_t size) { ranges.Add(device_addr, size); });
    }
    return ranges;
}

template <class P>
void BufferCache<P>::WriteDownloadedMemory(const Common::RangeSet<DAddr>& cpu_written,
                                           DAddr device_addr, const u8* data, u64 size) {
    if (!cpu_written.Intersects(device_addr, size)) {
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
    u64 written_bytes = 0;
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
            written_bytes += end - start;
        });
    }
    if (Nx2Profile::Enabled()) {
        Nx2Profile::g_fencing_written_bytes.fetch_add(written_bytes, std::memory_order_relaxed);
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
            std::array<BufferCopy, 1> copies{
                {BufferCopy{.src_offset = upload_staging.offset, .dst_offset = 0, .size = size}}};
            std::memcpy(upload_staging.mapped_span.data(),
                        draw_state.inline_index_draw_indexes.data(), size);
            runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true);
        } else {
            buffer.ImmediateUpload(0, draw_state.inline_index_draw_indexes);
        }
    }
    if constexpr (HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT) {
        const u32 new_offset = offset + draw_state.index_buffer.first *
                                            u32(draw_state.index_buffer.FormatSizeInBytes());
        runtime.BindIndexBuffer(buffer, new_offset, size);
    } else {
        buffer.MarkUsage(offset, size);
        runtime.BindIndexBuffer(draw_state.topology, draw_state.index_buffer.format,
                                draw_state.index_buffer.first, draw_state.index_buffer.count,
                                buffer, offset, size);
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
    RefreshRepackedVertexBuffers();

#ifdef __ANDROID__
    const bool use_optimized_vertex_buffers =
        Settings::values.use_optimized_vertex_buffers.GetValue();
#else
    constexpr bool use_optimized_vertex_buffers = true;
#endif

    if (use_optimized_vertex_buffers) {
        auto& flags = maxwell3d->dirty.flags;
        u32 enabled_mask = enabled_vertex_buffers_mask;
        HostBindings<Buffer> bindings;
        u32 last_index = (std::numeric_limits<u32>::max)();
        const auto flush_bindings = [&]() {
            if (bindings.buffers.empty()) {
                return;
            }
            bindings.max_index = bindings.min_index + static_cast<u32>(bindings.buffers.size());
            runtime.BindVertexBuffers(bindings);
            bindings.buffers.clear();
            bindings.offsets.clear();
            bindings.sizes.clear();
            bindings.strides.clear();
            bindings.min_index = NUM_VERTEX_BUFFERS;
            bindings.max_index = 0;
            last_index = (std::numeric_limits<u32>::max)();
        };
        while (enabled_mask != 0) {
            const u32 index = std::countr_zero(enabled_mask);
            enabled_mask &= (enabled_mask - 1);
            const Binding& binding = VertexBufferSlot(index);
            Buffer& buffer = slot_buffers[binding.buffer_id];
            TouchBuffer(buffer, binding.buffer_id);
            SynchronizeBuffer(buffer, binding.device_addr, binding.size);
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
        for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
            const Binding& binding = channel_state->vertex_buffers[index];
            Buffer& buffer = slot_buffers[binding.buffer_id];
            TouchBuffer(buffer, binding.buffer_id);
            SynchronizeBuffer(buffer, binding.device_addr, binding.size);
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
void BufferCache<P>::BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index,
                                                   bool needs_bind) {
    ++channel_state->uniform_cache_shots[0];
    // Bank 24 (root table 0, NVN2's push constants) is read from bank 17, which such games do not
    // bind: stream the table's current words as that buffer.
    if (index == 17 && maxwell3d->root_tables.loaded &&
        channel_state->uniform_buffers[stage][index].size == 0) {
        const auto& words = maxwell3d->root_tables.tables[0];
        const u32 size = (*channel_state->uniform_buffer_sizes)[stage][index];
        const u32 copied = (std::min)(size, static_cast<u32>(sizeof(words)));
        if constexpr (IS_OPENGL) {
            std::array<u8, sizeof(words)> data{};
            std::memcpy(data.data(), words.data(), copied);
            runtime.BindFastUniformBuffer(stage, binding_index, copied);
            runtime.PushFastUniformBuffer(stage, binding_index,
                                          std::span<const u8>(data.data(), copied));
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
    Buffer& buffer = slot_buffers[binding.buffer_id];
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
    // the memory tracker, leaving stale host contents, so upload such a bank again once per frame
    // by default (with 4 KiB page-level shadow diffing so unchanged pages stay clean).
    // EDEN_NX2_BANK_REUPLOAD: 0 off, 1 once per frame (the default), 2 every bind of a 64 KiB
    // bank, 3 every bind of any native bank.
    static const char bank_mode = [] {
        const char* v = std::getenv("EDEN_NX2_BANK_REUPLOAD");
        return v != nullptr ? v[0] : '1';
    }();
    if (bank_mode != '0' && has_host_buffer && size != 0 && gpu_memory != nullptr &&
        gpu_memory->UsesNativeTiling() &&
        (bank_mode == '3' || (*channel_state->uniform_buffer_sizes)[stage][index] == 0x10000)) {
        Nx2Profile::Scope bank_scope{Nx2Profile::Zone::BindUniformBank};
        // The bank size is only an upper limit: stop at the end of the mapped memory.
        // Only mark 4 KiB pages whose guest bytes actually changed since the last upload so
        // unchanged banks do not toggle VirtualProtect or break the Vulkan render pass on every draw.
        NativeUniformBankShadow* shadow = nullptr;
        if (device_addr == native_indirect_uniform_last_bank &&
            native_indirect_uniform_last_ptr != nullptr) [[likely]] {
            shadow = native_indirect_uniform_last_ptr;
        } else {
            const size_t cache_idx = ((device_addr >> 12) ^ (device_addr >> 20)) & 7;
            auto& cached = native_indirect_uniform_cache[cache_idx];
            if (cached.first == device_addr && cached.second != nullptr) [[likely]] {
                shadow = cached.second;
            } else {
                shadow = &native_indirect_uniform_shadow[device_addr];
                cached = {device_addr, shadow};
            }
            native_indirect_uniform_last_bank = device_addr;
            native_indirect_uniform_last_ptr = shadow;
        }
        // Nx2Perf bank_once_per_frame: compare the guest bytes on the frame's first bind only.
        // Verify (which always takes the per-draw path) reports changes that path would have
        // uploaded one frame late.
        const bool once_per_frame = bank_mode != '2' && bank_mode != '3' &&
                                    Nx2Perf::On(Nx2Perf::Opt::BankOncePerFrame);
        const bool skip_this_frame = once_per_frame && !Nx2Perf::Verify();
        const bool seen_this_frame = shadow->last_frame_tick == frame_tick;
        if (skip_this_frame && seen_this_frame) {
            if (Nx2Profile::Enabled() &&
                shadow->last_draw_ticket != native_indirect_uniform_draw_ticket &&
                shadow->last_skipped_draw_ticket != native_indirect_uniform_draw_ticket &&
                !memory_tracker.IsRegionGpuModified(device_addr, size)) {
                shadow->last_skipped_draw_ticket = native_indirect_uniform_draw_ticket;
                Nx2Profile::Add(Nx2Profile::Counter::BankRechecksSkippedByFrame, 1);
            }
        } else if (!memory_tracker.IsRegionGpuModified(device_addr, size)) {
            const bool check_late = once_per_frame && seen_this_frame;
            shadow->last_frame_tick = frame_tick;
            if (shadow->last_draw_ticket != native_indirect_uniform_draw_ticket) {
                shadow->last_draw_ticket = native_indirect_uniform_draw_ticket;
                const u32 check_size = (std::min<u32>)(size, 0x10000U);
                const u8* const base_ptr = device_memory.template GetPointer<u8>(device_addr);
                const bool contiguous =
                    base_ptr != nullptr &&
                    (check_size <= Core::DEVICE_PAGESIZE ||
                     base_ptr + (check_size - 1) ==
                         device_memory.template GetPointer<u8>(device_addr + check_size - 1));
                u32 offset_in_bank = 0;
                u32 page_idx = 0;
                while (offset_in_bank < check_size) {
                    const DAddr cur_addr = device_addr + offset_in_bank;
                    const u64 page_offset = cur_addr & Core::DEVICE_PAGEMASK;
                    const u32 chunk_size = static_cast<u32>((std::min<u64>)(
                        Core::DEVICE_PAGESIZE - page_offset, check_size - offset_in_bank));
                    const u8* const ptr = contiguous
                                              ? (base_ptr + offset_in_bank)
                                              : device_memory.template GetPointer<u8>(cur_addr);
                    if (ptr == nullptr) {
                        break;
                    }
                    const u32 page_bit = 1U << (page_idx & 31);
                    const bool page_initialized = (shadow->initialized_mask & page_bit) != 0;
                    const bool already_cpu_modified =
                        memory_tracker.IsRegionCpuModified(cur_addr, chunk_size);
                    u8* const shadow_dst = shadow->bytes.data() + offset_in_bank;
                    if (!page_initialized || already_cpu_modified ||
                        std::memcmp(shadow_dst, ptr, chunk_size) != 0) {
                        if (check_late && page_initialized && !already_cpu_modified)
                            [[unlikely]] {
                            static std::atomic<u64> late_changes{};
                            const u64 late =
                                late_changes.fetch_add(1, std::memory_order_relaxed) + 1;
                            Nx2Profile::Add(Nx2Profile::Counter::BankLateChanges, 1);
                            if (late <= 64) {
                                LOG_ERROR(HW_GPU,
                                          "Nx2Perf bank_once_per_frame: bank {:#x} page {} "
                                          "changed after the frame's first bind (frame {})",
                                          device_addr, page_idx, frame_tick);
                            } else if (late % 1024 == 0) {
                                LOG_ERROR(HW_GPU,
                                          "Nx2Perf bank_once_per_frame: {} bank pages changed "
                                          "after the frame's first bind so far",
                                          late);
                            }
                        }
                        std::memcpy(shadow_dst, ptr, chunk_size);
                        shadow->initialized_mask |= page_bit;
                        if (!already_cpu_modified) {
                            memory_tracker.MarkRegionAsCpuModified(cur_addr, chunk_size);
                            last_synced_count = 0;
                        }
                    }
                    offset_in_bank += chunk_size;
                    ++page_idx;
                }
            }
        }
    }
    const bool use_fast_buffer =
        needs_alignment_stream ||
        (has_host_buffer && size <= channel_state->uniform_buffer_skip_cache_size &&
         !memory_tracker.IsRegionGpuModified(device_addr, size));
    if (use_fast_buffer) {
        Nx2Profile::Scope stream_scope{Nx2Profile::Zone::BindUniformStream};
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
                streamed = VideoCommon::BindSmallUniformReusing(
                    runtime, device_memory, stage, index, binding_index, device_addr, size);
            }
        }
        if (!streamed) {
            const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            device_memory.ReadBlockUnsafe(device_addr, span.data(), size);
        }
        return;
    }
    Nx2Profile::Scope cached_scope{Nx2Profile::Zone::BindUniformCached};
    BindMemo* const memo =
        BindMemoEnabled() && has_host_buffer ? &uniform_bind_memo[stage][index] : nullptr;
    const bool memo_hit = memo != nullptr && BindMemoHit(*memo, buffer, binding.buffer_id,
                                                         device_addr, size, offset, "uniform",
                                                         stage, index);
    if (memo_hit) {
        // The sync would have found nothing to upload and returned true.
        ++channel_state->uniform_cache_hits[0];
    } else {
        const u64 cpu_epoch = memory_tracker.CpuEpoch();
        TouchBuffer(buffer, binding.buffer_id);
        // Classic cached path
        if (SynchronizeBuffer(buffer, device_addr, size)) {
            ++channel_state->uniform_cache_hits[0];
        }
        if (memo != nullptr) {
            BindMemoNote(*memo, binding.buffer_id, device_addr, size, cpu_epoch);
        }
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
    if (!memo_hit || !memo->usage_marked) {
        buffer.MarkUsage(offset, size);
        if (memo != nullptr) {
            BindMemoNoteUsage(*memo);
        }
    }
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
        if (binding.single_range || binding.gpu_addr == 0 || binding.size == 0) {
            return;
        }
        if (is_written && !runtime.PrefersSparseSources()) {
            return;
        }
        const VirtualSegments* found =
            virtual_ranges.Query(*gpu_memory, binding.gpu_addr, binding.size);
        if (!found || found->size() < 2) {
            if (found) {
                binding.single_range = true;
            }
            return;
        }
        const VirtualSegments segments = *found;
        const u32 first = static_cast<u32>(pool.size());
        const bool prefer_sparse = runtime.PrefersSparseSources();
        for (const VirtualSegment& segment : segments) {
            const BufferId buffer_id = FindBuffer(segment.device_addr, segment.size, prefer_sparse);
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
    nx2_graphics_stage = static_cast<int>(stage + 1);
    SCOPE_EXIT {
        nx2_graphics_stage = -1;
    };
    u32 binding_index = 0;
    ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
        Nx2Profile::Scope storage_scope{Nx2Profile::Zone::BindStorage};
        const Binding& binding = channel_state->storage_buffers[stage][index];
        const bool is_written = ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0;
        if (BindMultiRangeStorage(binding, is_written, graphics_segments)) {
            return;
        }
        Buffer& buffer = slot_buffers[binding.buffer_id];
        BindMemo* const memo = BindMemoEnabled() && binding.buffer_id != NULL_BUFFER_ID
                                   ? &storage_bind_memo[stage][index]
                                   : nullptr;
        const bool memo_hit =
            memo != nullptr && !is_written &&
            BindMemoHit(*memo, buffer, binding.buffer_id, binding.device_addr, binding.size,
                        buffer.Offset(binding.device_addr), "storage", stage, index) &&
            memo->usage_marked;
        const u64 cpu_epoch = memory_tracker.CpuEpoch();
        if (!memo_hit) {
            TouchBuffer(buffer, binding.buffer_id);
        }
        const u32 size = binding.size;
        if (!memo_hit) {
            SynchronizeBuffer(buffer, binding.device_addr, size);
        }

        const u32 offset = buffer.Offset(binding.device_addr);
        if (!memo_hit) {
            buffer.MarkUsage(offset, size);
            if (memo != nullptr && is_written) {
                memo->valid = false;
            } else if (memo != nullptr) {
                BindMemoNote(*memo, binding.buffer_id, binding.device_addr, size, cpu_epoch);
                BindMemoNoteUsage(*memo);
            }
        }

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
    nx2_graphics_stage = static_cast<int>(stage + 1);
    SCOPE_EXIT {
        nx2_graphics_stage = -1;
    };
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
                bool streamed = false;
                if constexpr (VideoCommon::StreamUniformReusable<Runtime>) {
                    if (VideoCommon::UniformStreamReuseEnabled()) {
                        streamed = VideoCommon::BindSmallComputeUniformReusing(
                            runtime, device_memory, index, binding_index, binding.device_addr,
                            size);
                    }
                }
                if (!streamed) {
                    const std::span<u8> span =
                        runtime.BindMappedUniformBuffer(0, binding_index, size);
                    device_memory.ReadBlockUnsafe(binding.device_addr, span.data(), size);
                }
                return;
            }
        }

        TouchBuffer(buffer, binding.buffer_id);
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
            UpdateIndexBuffer();
        }
        UpdateVertexBuffers();
        UpdateTransformFeedbackBuffers();
        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
            if (channel_state->enabled_uniform_buffer_masks[stage] != 0) {
                UpdateUniformBuffers(stage);
            }
            if (channel_state->enabled_storage_buffers[stage] != 0) {
                UpdateStorageBuffers(stage);
            }
            if (channel_state->enabled_texture_buffers[stage] != 0) {
                UpdateTextureBuffers(stage);
            }
        }
        if (current_draw_indirect) {
            UpdateDrawIndirect();
        }
    });
}

template <class P>
void BufferCache<P>::DoUpdateComputeBuffers() {
    compute_segments.clear();
    BufferOperations([&]() {
        if (channel_state->enabled_compute_uniform_buffer_mask != 0) {
            UpdateComputeUniformBuffers();
        }
        if (channel_state->enabled_compute_storage_buffers != 0) {
            UpdateComputeStorageBuffers();
        }
        if (channel_state->enabled_compute_texture_buffers != 0) {
            UpdateComputeTextureBuffers();
        }
    });
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
            inline_buffer_id = CreateBuffer(0, buffer_size, false);
        }
        if (slot_buffers[inline_buffer_id].SizeBytes() < buffer_size) [[unlikely]] {
            memory_tracker.BumpGpuEpoch();
            slot_buffers.erase(inline_buffer_id);
            inline_buffer_id = CreateBuffer(0, buffer_size, false);
        }
        channel_state->index_buffer = Binding{
            .device_addr = 0,
            .size = inline_index_size,
            .buffer_id = inline_buffer_id,
        };
        return;
    }

    const GPUVAddr gpu_addr_begin = index_buffer_ref.StartAddress();
    const GPUVAddr gpu_addr_end = index_buffer_ref.EndAddress();
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    const u32 draw_size = (index_buffer_ref.count + index_buffer_ref.first) *
                          u32(index_buffer_ref.FormatSizeInBytes());
    const u32 size = (std::min)(address_size, draw_size);
    if (size == 0) {
        channel_state->index_buffer = NULL_BINDING;
        return;
    }
    auto& current = channel_state->index_buffer;
    const u64 map_serial = gpu_memory->GetMapSerial();
    const std::optional<DAddr> device_addr =
        (current.gpu_addr == gpu_addr_begin && current.map_serial == map_serial &&
         current.device_addr != 0)
            ? std::optional<DAddr>{current.device_addr}
            : gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    if (!device_addr) {
        current = NULL_BINDING;
        return;
    }
    current = Binding{
        .device_addr = *device_addr,
        .gpu_addr = gpu_addr_begin,
        .map_serial = map_serial,
        .size = size,
        .buffer_id = FindBuffer(*device_addr, size, false),
    };
}

template <class P>
void BufferCache<P>::UpdateVertexBuffers() {
    auto& flags = maxwell3d->dirty.flags;
    if (!maxwell3d->dirty.flags[Dirty::VertexBuffers]) {
        return;
    }
    flags[Dirty::VertexBuffers] = false;

    const auto flags_words = std::bit_cast<std::array<u64, 4>>(flags);
    u32 dirty_vb_mask = static_cast<u32>(flags_words[0] >> Dirty::VertexBuffer0);
    while (dirty_vb_mask != 0) {
        const u32 index = static_cast<u32>(std::countr_zero(dirty_vb_mask));
        dirty_vb_mask &= dirty_vb_mask - 1;
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
    const u64 native_size = native ? maxwell3d->regs.NativeVertexStreamSize(index) : 0;
    if (native && array.enable != 0 && native_size > (std::numeric_limits<u32>::max)()) {
        throw std::runtime_error("Native vertex stream exceeds supported 32-bit buffer size");
    }
    const GPUVAddr gpu_addr_end = native ? gpu_addr_begin + native_size : limit.Address() + 1;
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    u32 size = address_size; // TODO: Analyze stride and number of vertices
    if (array.enable == 0 || size == 0) {
        channel_state->vertex_buffers[index] = NULL_BINDING;
        UpdateVertexBufferSlot(index, NULL_BINDING);
        return;
    }
    auto& current = channel_state->vertex_buffers[index];
    const u64 map_serial = gpu_memory->GetMapSerial();
    if (!channel_state->has_deleted_buffers && current.buffer_id &&
        current.buffer_id != NULL_BUFFER_ID && current.gpu_addr == gpu_addr_begin &&
        current.size == size && current.map_serial == map_serial && current.device_addr != 0 &&
        !Settings::IsGPUFenceBehaviorAccurate()) [[likely]] {
        return;
    }
    const std::optional<DAddr> device_addr =
        (current.gpu_addr == gpu_addr_begin && current.map_serial == map_serial &&
         current.device_addr != 0)
            ? std::optional<DAddr>{current.device_addr}
            : gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    if (!device_addr) {
        current = NULL_BINDING;
        UpdateVertexBufferSlot(index, NULL_BINDING);
        return;
    }
    if (!gpu_memory->IsWithinGPUAddressRange(gpu_addr_end) || size >= 64_MiB) {
        size = static_cast<u32>(gpu_memory->MaxContinuousRange(gpu_addr_begin, size));
    }
    const BufferId buffer_id = FindBuffer(*device_addr, size, false);
    const Binding binding{
        .device_addr = *device_addr,
        .gpu_addr = gpu_addr_begin,
        .map_serial = map_serial,
        .size = size,
        .buffer_id = buffer_id,
    };
    current = binding;
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
    const u32 to_resolve = channel_state->enabled_uniform_buffer_masks[stage] &
                           channel_state->unresolved_uniform_buffers[stage];
    if (to_resolve == 0) [[likely]] {
        return;
    }
    channel_state->unresolved_uniform_buffers[stage] &= ~to_resolve;
    ForEachEnabledBit(to_resolve, [&](u32 index) {
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
        if (!binding.buffer_id) {
            binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
        }
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
    const u64 map_serial = gpu_memory->GetMapSerial();
    ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
        Binding& binding = channel_state->compute_uniform_buffers[index];
        const auto& launch_desc = kepler_compute->launch_description;
        if (((launch_desc.const_buffer_enable_mask >> index) & 1) != 0) {
            const auto& cbuf = launch_desc.const_buffer_config[index];
            const GPUVAddr gpu_addr = cbuf.Address();
            if (!channel_state->has_deleted_buffers && binding.buffer_id &&
                binding.buffer_id != NULL_BUFFER_ID && binding.gpu_addr == gpu_addr &&
                binding.size == cbuf.size && binding.map_serial == map_serial &&
                binding.device_addr != 0) [[likely]] {
                return;
            }
            const std::optional<DAddr> device_addr =
                (binding.gpu_addr == gpu_addr && binding.map_serial == map_serial &&
                 binding.device_addr != 0)
                    ? std::optional<DAddr>{binding.device_addr}
                    : gpu_memory->GpuToCpuAddress(gpu_addr);
            if (device_addr) {
                if (!channel_state->has_deleted_buffers && binding.buffer_id &&
                    binding.buffer_id != NULL_BUFFER_ID &&
                    binding.device_addr == *device_addr && binding.size == cbuf.size) {
                    binding.gpu_addr = gpu_addr;
                    binding.map_serial = map_serial;
                    return;
                }
                binding.device_addr = *device_addr;
                binding.gpu_addr = gpu_addr;
                binding.map_serial = map_serial;
                binding.size = cbuf.size;
                binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
                return;
            }
        }
        binding = NULL_BINDING;
    });
}

template <class P>
void BufferCache<P>::UpdateComputeStorageBuffers() {
    ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
        // Resolve buffer
        Binding& binding = channel_state->compute_storage_buffers[index];
        if (!binding.buffer_id) {
            binding.buffer_id = FindBuffer(binding.device_addr, binding.size, false);
        }
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
void BufferCache<P>::MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size,
                                       bool needs_sync) {
    if constexpr (!IS_OPENGL) {
        if (needs_sync) {
            Buffer& buffer = slot_buffers[buffer_id];
            buffer.setWriteTick(runtime.CurrentTick());
        }
    }
    // The downloads already taken over this range hold older bytes: when their fences pop
    // they must not write them over this write's result in guest memory, nor drop the range
    // from gpu_modified_ranges (the next fence downloads it instead).
    for (auto& ranges : pending_download_ranges) {
        ranges.Subtract(device_addr, size);
    }
    memory_tracker.MarkRegionAsGpuModified(device_addr, size); // Bumps the GPU epoch
    MarkVertexBuffersStale(device_addr, size);
    last_synced_count = 0;
    gpu_modified_ranges.Add(device_addr, size);
    QueueGpuWrite(device_addr, size);
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
                                         "lists wait for the work before the last host sync",
                                 page_addr);
                    }
                }
                if (nx2_collect_list_copies) {
                    if (nx2_list_copy_requests.empty() ||
                        nx2_list_copy_requests.back().second != page_addr) {
                        nx2_list_copy_requests.emplace_back(buffer_id, page_addr);
                    }
                }
                if (nx2_graphics_stage >= 0 && maxwell3d != nullptr) {
                    Tegra::Nx2MarkGraphicsWriterShader(
                        maxwell3d->regs.pipelines[nx2_graphics_stage].reserved[1]);
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
            const DAddr page = nx2_list_copy_requests[i].second;
            if (i + 1 < nx2_list_copy_requests.size() &&
                nx2_list_copy_requests[i + 1].second == page) {
                continue;
            }
            const u64 page_index = page >> CACHING_PAGEBITS;
            if (page_index >= page_table.size()) {
                continue;
            }
            const BufferId live_id = page_table[page_index];
            if (!live_id) {
                continue;
            }
            Buffer& buffer = slot_buffers[live_id];
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
            entry.copy_time = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
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
auto BufferCache<P>::Nx2ReadListCopy(DAddr device_addr, std::span<u8> out, bool past_sync)
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
            return entry.copy && entry.copy_writes == entry.writes &&
                   offset >= entry.copy_begin && offset + out.size() <= entry.copy_end;
        };
        u64 tick{};
        u64 writes{};
        u64 copy_time{};
        {
            std::scoped_lock lock{nx2_list_mutex};
            const auto it = nx2_list_pages.find(page);
            if (it == nx2_list_pages.end()) {
                return Nx2ListRead::Unknown;
            }
            const Nx2ListPage& entry = it->second;
            if (entry.draw_written) {
                // EDEN_NX2_CMDPAGE_DRAIN=1: drain and flush for every list on such pages.
                static const bool always_drain = std::getenv("EDEN_NX2_CMDPAGE_DRAIN") != nullptr;
                if (always_drain) {
                    return Nx2ListRead::Unknown;
                }
                if (!past_sync) {
                    return Nx2ListRead::NeedsSync;
                }
                return entry.synced_writes == entry.writes ? Nx2ListRead::Synced
                                                           : Nx2ListRead::Unknown;
            }
            // Pages the CPU writes the lists of (GPU-written once, long before) stay synced.
            if (entry.synced_writes == entry.writes) {
                return Nx2ListRead::Synced;
            }
            if (!current(entry)) {
                return Nx2ListRead::Unknown;
            }
            // The lists after the first of a batch find the copy done: read it at once.
            if (runtime.IsFree(entry.copy_tick)) {
                std::memcpy(out.data(), entry.copy->mapped_span.data() + offset, out.size());
                return Nx2ListRead::Copied;
            }
            tick = entry.copy_tick;
            writes = entry.copy_writes;
            copy_time = entry.copy_time;
        }
        // Submitted with the work that wrote the page (Nx2CopyListPages's caller flushes).
        {
            Nx2Profile::Scope profile_scope{Nx2Profile::Zone::ListCopyWait};
            runtime.WaitSubmittedTick(tick);
        }
        Nx2Profile::AddListCopyDone(copy_time);
        std::scoped_lock lock{nx2_list_mutex};
        const auto it = nx2_list_pages.find(page);
        if (it == nx2_list_pages.end() || !current(it->second) || it->second.copy_tick != tick ||
            it->second.copy_writes != writes) {
            return Nx2ListRead::Unknown;
        }
        std::memcpy(out.data(), it->second.copy->mapped_span.data() + offset, out.size());
        return Nx2ListRead::Copied;
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
void BufferCache<P>::Nx2NoteListHostWrite(DAddr device_addr, u64 size, std::span<const u8> data) {
    if constexpr (!IS_OPENGL) {
        if (size == 0 || !Tegra::g_nx2_has_command_list_pages.load(std::memory_order_relaxed)) {
            return;
        }
        constexpr u64 page_bits = Tegra::Nx2WrittenPageBits;
        constexpr u64 page_size = u64{1} << page_bits;
        const u64 last = (std::min<u64>)((device_addr + size - 1) >> page_bits,
                                         Tegra::g_nx2_command_list_pages.size() * 64 - 1);
        for (u64 page = device_addr >> page_bits; page <= last; ++page) {
            if (!((Tegra::g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >>
                   (page % 64)) &
                  1)) {
                continue;
            }
            std::scoped_lock lock{nx2_list_mutex};
            const DAddr page_addr = page << page_bits;
            const auto it = nx2_list_pages.find(page_addr);
            if (it == nx2_list_pages.end()) {
                continue;
            }
            Nx2ListPage& entry = it->second;
            const bool synced = entry.synced_writes == entry.writes;
            const bool copy_valid = entry.copy && entry.copy_writes == entry.writes;
            ++entry.writes;
            if (synced) {
                entry.synced_writes = entry.writes;
            }
            bool copy_updated = false;
            if (!data.empty() && copy_valid && runtime.IsFree(entry.copy_tick)) {
                const u64 range_start = (std::max<u64>)(device_addr, page_addr);
                const u64 range_end = (std::min<u64>)(device_addr + size, page_addr + page_size);
                if (range_start < range_end) {
                    const u64 offset_in_page = range_start - page_addr;
                    const u64 offset_in_src = range_start - device_addr;
                    const u64 copy_len = range_end - range_start;
                    if (offset_in_page >= entry.copy_begin &&
                        offset_in_page + copy_len <= entry.copy_end &&
                        offset_in_src + copy_len <= data.size()) {
                        std::memcpy(entry.copy->mapped_span.data() + offset_in_page,
                                    data.data() + offset_in_src, copy_len);
                        entry.copy_writes = entry.writes;
                        copy_updated = true;
                    }
                }
            }
            if (!synced && !copy_updated) {
                const u64 page_idx = page_addr >> CACHING_PAGEBITS;
                if (page_idx < page_table.size()) {
                    const BufferId live_id = page_table[page_idx];
                    if (live_id) {
                        if (nx2_list_copy_requests.empty() ||
                            nx2_list_copy_requests.back().second != page_addr) {
                            nx2_list_copy_requests.emplace_back(live_id, page_addr);
                        }
                    }
                }
            }
        }
    }
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
    runtime.CopyBuffer(new_buffer, overlap, copies, true);
#ifdef YUZU_LEGACY
    if (immediately_free)
        runtime.Finish();
#endif
    DeleteBuffer(overlap_id, true);
}

template <class P>
BufferId BufferCache<P>::CreateBuffer(DAddr device_addr, u32 wanted_size, bool sparse_compatible) {
    DAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    memory_tracker.BumpGpuEpoch();
    const BufferId new_buffer_id =
        slot_buffers.insert(runtime, overlap.begin, size, sparse_compatible);
    auto& new_buffer = slot_buffers[new_buffer_id];
    const size_t size_bytes = new_buffer.SizeBytes();
    runtime.ClearBuffer(new_buffer, 0, size_bytes, 0);
    new_buffer.MarkUsage(0, size_bytes);
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
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
        lru_cache.Free(buffer.getLRUID());
    }
    const DAddr device_addr_begin = buffer.CpuAddr();
    const DAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page] = buffer_id;
        } else {
            page_table[page] = BufferId{};
        }
    }
}

template <class P>
bool BufferCache<P>::BindMemoEnabled() {
    static const bool enabled = !IS_OPENGL && Nx2Perf::On(Nx2Perf::Opt::BindMemo);
    return enabled;
}

template <class P>
bool BufferCache<P>::BindMemoHit(const BindMemo& memo, Buffer& buffer, BufferId buffer_id,
                                 DAddr device_addr, u32 size, u32 offset, const char* kind,
                                 size_t stage, u32 index) {
    if constexpr (IS_OPENGL) {
        return false;
    } else {
        if (!memo.valid || memo.channel != channel_state || memo.buffer_id != buffer_id ||
            memo.device_addr != device_addr || memo.size != size ||
            memo.cpu_epoch != memory_tracker.CpuEpoch() ||
            memo.gpu_epoch != memory_tracker.GpuEpoch() || memo.frame != frame_tick ||
            memo.tick != runtime.CurrentTick()) {
            Nx2Profile::Add(Nx2Profile::Counter::BindMemoMisses, 1);
            return false;
        }
        Nx2Profile::Add(Nx2Profile::Counter::BindMemoHits, 1);
        if (!Nx2Perf::Verify()) {
            return true;
        }
        // Verify: the skipped sync must find nothing to upload, and the skipped usage mark must
        // already be in place. The full path runs either way.
        const auto mismatch = [&](std::string_view what) {
            if (bind_memo_mismatches++ < 64) {
                LOG_ERROR(HW_GPU,
                          "Nx2Perf bind_memo: {} {} stage {} index {} at {:#x}+{:#x} "
                          "(mismatches {})",
                          kind, what, stage, index, device_addr, size, bind_memo_mismatches);
            }
        };
        if (memory_tracker.IsRegionCpuModified(device_addr, size)) {
            mismatch("sync would upload");
        }
        if (memo.usage_marked && size != 0 && !buffer.IsRegionUsed(offset, size)) {
            mismatch("usage not marked");
        }
        return false;
    }
}

template <class P>
void BufferCache<P>::BindMemoNote(BindMemo& memo, BufferId buffer_id, DAddr device_addr, u32 size,
                                  u64 cpu_epoch_before) {
    if constexpr (!IS_OPENGL) {
        // A walk of the range leaves it clean unless something marked it meanwhile; a deduped
        // sync did not walk it, so check.
        if (memory_tracker.CpuEpoch() != cpu_epoch_before ||
            (last_sync_deduped && memory_tracker.IsRegionCpuModified(device_addr, size))) {
            memo.valid = false;
            return;
        }
        memo = BindMemo{
            .channel = channel_state,
            .buffer_id = buffer_id,
            .device_addr = device_addr,
            .size = size,
            .valid = true,
            .usage_marked = false,
            .cpu_epoch = cpu_epoch_before,
            .gpu_epoch = memory_tracker.GpuEpoch(),
            .tick = runtime.CurrentTick(),
            .frame = frame_tick,
        };
    }
}

template <class P>
void BufferCache<P>::BindMemoNoteUsage(BindMemo& memo) {
    if constexpr (!IS_OPENGL) {
        if (memo.valid && memo.tick == runtime.CurrentTick() && memo.frame == frame_tick) {
            memo.usage_marked = true;
        }
    }
}

template <class P>
bool BufferCache<P>::SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size) {
    last_sync_deduped = false;
    if (size == 0) {
        return true;
    }
    const DAddr range_end = device_addr + size;
    if (native_indirect_uniform_draw_ticket != 0) {
        if (last_synced_draw_ticket != native_indirect_uniform_draw_ticket) {
            last_synced_draw_ticket = native_indirect_uniform_draw_ticket;
            last_synced_count = 0;
        } else {
            for (u32 i = 0; i < last_synced_count; ++i) {
                if (last_synced_ranges[i].begin <= device_addr &&
                    range_end <= last_synced_ranges[i].end) [[likely]] {
                    last_sync_deduped = true;
                    return true;
                }
            }
        }
    }
    const auto note_synced_range = [&]() {
        if (native_indirect_uniform_draw_ticket == 0) {
            return;
        }
        for (u32 i = 0; i < last_synced_count; ++i) {
            if (device_addr <= last_synced_ranges[i].end &&
                range_end >= last_synced_ranges[i].begin) {
                last_synced_ranges[i].begin = (std::min)(last_synced_ranges[i].begin, device_addr);
                last_synced_ranges[i].end = (std::max)(last_synced_ranges[i].end, range_end);
                return;
            }
        }
        if (last_synced_count < last_synced_ranges.size()) {
            last_synced_ranges[last_synced_count++] = LastSyncedRange{device_addr, range_end};
        }
    };
    // An upload over a clean word of the tracker (word_manager.h) finds and changes nothing:
    // walk only the dirty ones. Verify walks every word and checks the clean words.
    const bool skip_clean = Nx2Perf::On(Nx2Perf::Opt::CleanWords) && !Nx2Perf::Verify();
    if (Nx2Perf::On(Nx2Perf::Opt::CleanWords) && Nx2Perf::Verify()) {
        static std::atomic<u32> logged{};
        if (!memory_tracker.CleanWordsHold(device_addr, size) &&
            logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU, "Nx2Perf draw-binds: clean tracker words wrong near {:#x}+{:#x}",
                      device_addr, size);
        }
    }
    const auto for_each_upload_range = [&](auto&& func) {
        if (skip_clean) {
            memory_tracker.ForEachDirtyUploadRange(device_addr, size, func);
        } else {
            memory_tracker.ForEachUploadRange(device_addr, size, func);
        }
    };
#ifdef __ANDROID__
    const bool use_optimized_vertex_buffers =
        Settings::values.use_optimized_vertex_buffers.GetValue();
#else
    constexpr bool use_optimized_vertex_buffers = true;
#endif

    if (use_optimized_vertex_buffers) {
        upload_copies.clear();
        u64 total_size_bytes = 0;
        u64 largest_copy = 0;
        const DAddr buffer_start = buffer.cpu_addr_cached;
        const auto add_upload = [&](DAddr start, DAddr end) {
            if (start == end)
                return;
            const u64 range_size = end - start;
            upload_copies.push_back(BufferCopy{
                .src_offset = total_size_bytes,
                .dst_offset = start - buffer_start,
                .size = range_size,
            });
            total_size_bytes += range_size;
            largest_copy = (std::max)(largest_copy, range_size);
        };
        for_each_upload_range([&](u64 device_addr_out, u64 range_size) {
            DAddr upload_start = device_addr_out;
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, [&](DAddr gpu_start, DAddr gpu_end) {
                add_upload(upload_start, gpu_start);
                upload_start = gpu_end;
            });
            add_upload(upload_start, device_addr_out + range_size);
        });
        note_synced_range();
        if (total_size_bytes == 0) {
            return true;
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
        for_each_upload_range([&](u64 device_addr_out, u64 range_size) {
            copies.push_back(BufferCopy{
                .src_offset = total_size_bytes,
                .dst_offset = device_addr_out - buffer_start,
                .size = range_size,
            });
            total_size_bytes += range_size;
            largest_copy = (std::max)(largest_copy, range_size);
        });
        note_synced_range();
        if (total_size_bytes == 0) {
            return true;
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
            // Large uploads are mostly geometry the host GPU now has its own copy of: TickFrame
            // marks their guest memory cold (see TextureCache::RefreshContents).
            if (copy.size >= 256_KiB) {
                uploaded_buffer_ranges.emplace_back(device_addr, copy.size);
            }
            // Apply the staging offset
            copy.src_offset += upload_staging.offset;
        }
        const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true, can_reorder);
    }
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
    last_synced_count = 0;

    BufferId buffer_id = FindBuffer(dest_address, static_cast<u32>(copy_size), false);
    auto& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, dest_address, static_cast<u32>(copy_size));

    if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
        auto upload_staging = runtime.UploadStagingBuffer(copy_size);
        std::array copies{BufferCopy{
            .src_offset = upload_staging.offset,
            .dst_offset = buffer.Offset(dest_address),
            .size = copy_size,
        }};
        u8* const src_pointer = upload_staging.mapped_span.data();
        std::memcpy(src_pointer, inlined_buffer.data(), copy_size);
        const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
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
                gpu_modified_ranges.ForEachInRange(
                    device_addr_out, range_size, [&](DAddr start, DAddr end) {
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
        const DAddr device_addr =
            slot_buffers[download.buffer_id].CpuAddr() + download.copy.src_offset;
        WriteDownloadedMemory(cpu_written, device_addr,
                              mapped_memory + (download.copy.dst_offset - download_staging.offset),
                              download.copy.size);
    }
}

template <class P>
void BufferCache<P>::DownloadBufferMemory(Buffer& buffer, DAddr device_addr, u64 size) {
    boost::container::small_vector<BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    memory_tracker.ForEachDownloadRangeAndClear(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const DAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](DAddr start, DAddr end) {
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
    if (total_size_bytes == 0) {
        return;
    }
    const Common::RangeSet<DAddr> cpu_written = PendingCpuWriteRanges();

    if constexpr (requires { buffer.Mapped(); }) {
        if (const auto mapped = buffer.Mapped(); !mapped.empty()) {
            runtime.Wait(buffer.LastUsageTick());
            for (const BufferCopy& copy : copies) {
                WriteDownloadedMemory(cpu_written, buffer.CpuAddr() + copy.src_offset,
                                      mapped.data() + copy.src_offset, copy.size);
            }
            return;
        }
    }
    if constexpr (USE_MEMORY_MAPS) {
        auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes);
        const u8* const mapped_memory = download_staging.mapped_span.data();
        const std::span<BufferCopy> copies_span(copies.data(), copies.data() + copies.size());
        for (BufferCopy& copy : copies) {
            // Modify copies to have the staging offset in mind
            copy.dst_offset += download_staging.offset;
            buffer.MarkUsage(copy.src_offset, copy.size);
        }
        runtime.CopyBuffer(download_staging.buffer, buffer, copies_span, true);
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
            WriteDownloadedMemory(cpu_written, copy_device_addr, immediate_buffer.data(),
                                  copy.size);
        }
    }
}

template <class P>
void BufferCache<P>::DeleteBuffer(BufferId buffer_id, bool do_not_mark) {
    memory_tracker.BumpGpuEpoch();
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

    for (u32 index = 0; index < channel_state->vertex_buffers.size(); index++) {
        auto& binding = channel_state->vertex_buffers[index];
        if (binding.buffer_id == buffer_id) {
            binding.buffer_id = BufferId{};
            UpdateVertexBufferSlot(index, binding);
            dirty_vertex_buffers.push_back(index);
        }
    }
    std::ranges::for_each(channel_state->uniform_buffers, replace);
    channel_state->unresolved_uniform_buffers.fill(~u32{0});
    std::ranges::for_each(channel_state->storage_buffers, replace);
    replace(channel_state->transform_feedback_buffers);
    replace(channel_state->compute_uniform_buffers);
    replace(channel_state->compute_storage_buffers);

    // The other channels keep their own bindings, and the resolved buffer ids survive across
    // draws there. Left alone, a stale id would address whatever buffer reuses this slot next
    // (Ocarina of Time draws from several channels: descriptors ended up ~100 MiB past 128 KiB
    // buffers and the GPU faulted).
    for (const size_t other_id : active_channel_ids) {
        BufferCacheChannelInfo& other = channel_storage[other_id];
        if (&other == channel_state) {
            continue;
        }
        bool other_dirty_index = false;
        if (other.index_buffer.buffer_id == buffer_id) {
            other.index_buffer.buffer_id = BufferId{};
            other_dirty_index = true;
        }
        auto& other_flags = other.maxwell3d.dirty.flags;
        for (u32 index = 0; index < other.vertex_buffers.size(); index++) {
            if (other.vertex_buffers[index].buffer_id == buffer_id) {
                other.vertex_buffers[index].buffer_id = BufferId{};
                other_flags[Dirty::VertexBuffers] = true;
                other_flags[Dirty::VertexBuffer0 + index] = true;
            }
        }
        if (other_dirty_index) {
            other_flags[Dirty::IndexBuffer] = true;
        }
        std::ranges::for_each(other.uniform_buffers, replace);
        other.unresolved_uniform_buffers.fill(~u32{0});
        std::ranges::for_each(other.storage_buffers, replace);
        replace(other.transform_feedback_buffers);
        replace(other.compute_uniform_buffers);
        replace(other.compute_storage_buffers);
        if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
            other.dirty_uniform_buffers.fill(~u32{0});
            other.uniform_buffer_binding_sizes.fill({});
        }
        other.has_deleted_buffers = true;
    }

    // Mark the whole buffer as CPU written to stop tracking CPU writes
    if (!do_not_mark) {
        Buffer& buffer = slot_buffers[buffer_id];
        memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddr(), buffer.SizeBytes());
        last_synced_count = 0;
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
Binding BufferCache<P>::StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index, bool is_written,
                                             bool native_pointer, bool table_pointer) const {
    const GPUVAddr gpu_addr = gpu_memory->Read<u64>(ssbo_addr);

    if (gpu_addr == 0) {
        return NULL_BINDING;
    }

    const u8 cache_flags = static_cast<u8>((is_written ? 1u : 0u) |
                                           (native_pointer ? 2u : 0u) |
                                           (table_pointer ? 4u : 0u));
    const u64 map_serial = gpu_memory->GetMapSerial();
    const size_t memory_id = gpu_memory->GetID();
    const size_t cache_slot =
        (((gpu_addr >> 4) ^ (gpu_addr >> 20) ^ (u64{cache_flags} * 0x9e3779b9ULL)) &
         (ResolvedStorageAddrCacheSize - 1));
    if (native_pointer || table_pointer) {
        const auto& entry = resolved_storage_addr_cache[cache_slot];
        if (entry.valid && entry.gpu_addr == gpu_addr && entry.map_serial == map_serial &&
            entry.memory_id == memory_id && entry.flags == cache_flags) {
            return Binding{
                .device_addr = entry.device_addr,
                .gpu_addr = entry.aligned_gpu_addr,
                .size = entry.binding_size,
                .buffer_id = BufferId{},
                .single_range = entry.single_range,
            };
        }
    }

    u32 size = [&]() {
        const u32 memory_layout_size = static_cast<u32>((std::min)(
            u64{gpu_memory->GetMemoryLayoutSize(gpu_addr)}, u64{(std::numeric_limits<u32>::max)()}));
        // A pointer read from a table in memory is the base of an object pool, read anywhere in
        // it: bound to the end of its mapping, as far as a storage buffer may reach (at most
        // 256 MiB). Ocarina of Time's grass instances lie 12-18 MiB past theirs (an 8 MiB cap
        // culled every blade), and its light probe records reach about 130 MiB past theirs: a
        // 64 MiB cap left the far probes outside the binding, and the fog took one overbright
        // constant from them. Written pools keep a cap: a write marks the whole binding
        // GPU-modified.
        if (table_pointer && !is_written) {
            const u64 reach = (std::min)(runtime.GetMaxStorageBufferRange(), u64{256_MiB + 64_KiB});
            const u32 limit = static_cast<u32>(reach - (std::min)(reach, u64{64_KiB}));
            return (std::min)(memory_layout_size, limit);
        }
        // A native (Switch 2) compute program's pointer is bare: the word after it is another
        // parameter (a dimension, stride or texture handle), not an NVN buffer size. Taking it as
        // one bound Fortune's Weave's DLSS input features to 266 bytes and dropped the rest.
        // Only DLSS passes size from the mapping: doing it for every native dispatch binds up to
        // 8 MiB per buffer and slows Hyrule Warriors' fights to a few fps, then hangs them.
        if (native_pointer) {
            return (std::min)(memory_layout_size, static_cast<u32>(8_MiB));
        }
        const u64 next_qword = gpu_memory->Read<u64>(ssbo_addr + 8);
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
        // Stop where the mapping or its device range does: the binding is one device range, and
        // a written one isn't split into separate ranges without sparse sources.
        size = static_cast<u32>(gpu_memory->MaxContinuousRange(gpu_addr, size));
    }

    // Alignment only applies to the offset of the buffer
    const u32 alignment = runtime.GetStorageBufferAlignment();
    const GPUVAddr aligned_gpu_addr = Common::AlignDown(gpu_addr, alignment);
    const u32 aligned_size = static_cast<u32>(gpu_addr - aligned_gpu_addr) + size;

    const std::optional<DAddr> aligned_device_addr = gpu_memory->GpuToCpuAddress(aligned_gpu_addr);
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
    const bool single_range =
        (native_pointer || table_pointer) &&
        (aligned_gpu_addr >> Core::DEVICE_PAGEBITS) == (gpu_addr >> Core::DEVICE_PAGEBITS);
    const Binding binding{
        .device_addr = *aligned_device_addr,
        .gpu_addr = aligned_gpu_addr,
        .size = binding_size,
        .buffer_id = BufferId{},
        .single_range = single_range,
    };
    if (native_pointer || table_pointer) {
        resolved_storage_addr_cache[cache_slot] = ResolvedStorageAddrCacheEntry{
            .gpu_addr = gpu_addr,
            .aligned_gpu_addr = aligned_gpu_addr,
            .device_addr = *aligned_device_addr,
            .map_serial = map_serial,
            .memory_id = memory_id,
            .binding_size = binding_size,
            .flags = cache_flags,
            .single_range = single_range,
            .valid = true,
        };
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
