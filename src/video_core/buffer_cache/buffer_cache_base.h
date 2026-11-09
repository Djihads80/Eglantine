// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include "common/container/unordered_map.h"

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include "common/range_sets.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "common/spin_recursive_mutex.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/buffer_cache/buffer_base.h"
#include "video_core/buffer_cache/read_trap_tracker.h"
#include "video_core/buffer_cache/virtual_range_cache.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

using BufferId = Common::SlotId;

using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

#ifdef __APPLE__
constexpr u32 NUM_VERTEX_BUFFERS = 16;
#else
constexpr u32 NUM_VERTEX_BUFFERS = 32;
#endif
constexpr u32 NUM_TRANSFORM_FEEDBACK_BUFFERS = 4;
constexpr u32 NUM_GRAPHICS_UNIFORM_BUFFERS = 18;
constexpr u32 NUM_COMPUTE_UNIFORM_BUFFERS = 8;
constexpr u32 NUM_STORAGE_BUFFERS = Shader::Info::MAX_SSBOS;
constexpr u32 NUM_COMPUTE_STORAGE_BUFFERS = NUM_STORAGE_BUFFERS;
constexpr u32 NUM_TEXTURE_BUFFERS = 32;
constexpr u32 NUM_STAGES = 5;

static_assert(NUM_GRAPHICS_UNIFORM_BUFFERS <= 32, "fast bitmask must fit u32");
static_assert(NUM_STORAGE_BUFFERS <= 64, "storage binding masks must fit u64");

using UniformBufferSizes = std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>;
using ComputeUniformBufferSizes = std::array<u32, NUM_COMPUTE_UNIFORM_BUFFERS>;

struct Nx2ResetRun {
    GPUVAddr gpu_addr{};
    DAddr cpu_addr{};
    u32 count{};
    bool uploaded{};
};

enum class ObtainBufferSynchronize : u32 {
    NoSynchronize = 0,
    FullSynchronize = 1,
    SynchronizeNoDirty = 2,
};

enum class ObtainBufferOperation : u32 {
    DoNothing = 0,
    MarkAsWritten = 1,
    DiscardWrite = 2,
    MarkQuery = 3,
};

static constexpr BufferId NULL_BUFFER_ID{0};
static constexpr u32 DEFAULT_SKIP_CACHE_SIZE = static_cast<u32>(4_KiB);

struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u64 map_serial{};
    u32 size{};
    BufferId buffer_id;
    u32 segment_first{};
    u32 segment_count{};
    bool single_range{};
};

struct MultiRangeSegment {
    BufferId buffer_id;
    DAddr device_addr{};
    u32 size{};
};

struct TextureBufferBinding : Binding {
    PixelFormat format;
};

static constexpr Binding NULL_BINDING{
    .device_addr = 0,
    .size = 0,
    .buffer_id = NULL_BUFFER_ID,
};

template <typename Buffer>
struct HostBindings {
    boost::container::static_vector<Buffer*, NUM_VERTEX_BUFFERS> buffers;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> offsets;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> sizes;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> strides;
    u32 min_index{NUM_VERTEX_BUFFERS};
    u32 max_index{0};
};

class BufferCacheChannelInfo : public ChannelInfo {
public:
    BufferCacheChannelInfo() = delete;
    BufferCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept : ChannelInfo(state) {}
    BufferCacheChannelInfo(const BufferCacheChannelInfo& state) = delete;
    BufferCacheChannelInfo& operator=(const BufferCacheChannelInfo&) = delete;

    Binding index_buffer;
    std::array<Binding, NUM_VERTEX_BUFFERS> vertex_buffers;
    std::array<std::array<Binding, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES> uniform_buffers;
    std::array<std::array<Binding, NUM_STORAGE_BUFFERS>, NUM_STAGES> storage_buffers;
    std::array<std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS>, NUM_STAGES> texture_buffers;
    std::array<Binding, NUM_TRANSFORM_FEEDBACK_BUFFERS> transform_feedback_buffers;
    Binding count_buffer_binding;
    Binding indirect_buffer_binding;

    std::array<Binding, NUM_COMPUTE_UNIFORM_BUFFERS> compute_uniform_buffers;
    std::array<Binding, NUM_COMPUTE_STORAGE_BUFFERS> compute_storage_buffers;
    std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS> compute_texture_buffers;

    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    u32 enabled_compute_uniform_buffer_mask = 0;

    const UniformBufferSizes* uniform_buffer_sizes{};
    const ComputeUniformBufferSizes* compute_uniform_buffer_sizes{};

    std::array<u64, NUM_STAGES> enabled_storage_buffers{};
    std::array<u64, NUM_STAGES> written_storage_buffers{};
    u64 enabled_compute_storage_buffers = 0;
    u64 written_compute_storage_buffers = 0;

    u32 total_graphics_storage_buffers = 0;
    u32 total_compute_storage_buffers = 0;

    std::array<u32, NUM_STAGES> enabled_texture_buffers{};
    std::array<u32, NUM_STAGES> written_texture_buffers{};
    std::array<u32, NUM_STAGES> image_texture_buffers{};
    u32 enabled_compute_texture_buffers = 0;
    u32 written_compute_texture_buffers = 0;
    u32 image_compute_texture_buffers = 0;

    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_hits{};
    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_shots{};

    u32 uniform_buffer_skip_cache_size = DEFAULT_SKIP_CACHE_SIZE;

    bool has_deleted_buffers = false;

    std::array<u32, NUM_STAGES> dirty_uniform_buffers{};
    std::array<u32, NUM_STAGES> unresolved_uniform_buffers{
        ~u32{0}, ~u32{0}, ~u32{0}, ~u32{0}, ~u32{0},
    };
    std::array<u32, NUM_STAGES> fast_bound_uniform_buffers{};
    std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        uniform_buffer_binding_sizes{};
};

template <class P>
class BufferCache : public VideoCommon::ChannelSetupCaches<BufferCacheChannelInfo> {
    // Page size for caching purposes.
    // This is unrelated to the CPU page size and it can be changed as it seems optimal.
#ifdef YUZU_LEGACY
    static constexpr u32 CACHING_PAGEBITS = 12;
#else
    static constexpr u32 CACHING_PAGEBITS = 16;
#endif
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;

    static constexpr bool IS_OPENGL = P::IS_OPENGL;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS =
        P::HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT =
        P::HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = P::NEEDS_BIND_UNIFORM_INDEX;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = P::NEEDS_BIND_STORAGE_INDEX;
    static constexpr bool USE_MEMORY_MAPS = P::USE_MEMORY_MAPS;
    static constexpr bool SEPARATE_IMAGE_BUFFERS_BINDINGS = P::SEPARATE_IMAGE_BUFFER_BINDINGS;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = P::USE_MEMORY_MAPS_FOR_UPLOADS;

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 512_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB;

    // Debug Flags.

    static constexpr bool DISABLE_DOWNLOADS = true;

    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    using Runtime = typename P::Runtime;
    using Buffer = typename P::Buffer;
    using Async_Buffer = typename P::Async_Buffer;
    using MemoryTracker = typename P::MemoryTracker;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        DAddr begin;
        DAddr end;
        bool has_stream_leap = false;
    };

public:
    explicit BufferCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, Runtime& runtime_);

    ~BufferCache();

    void TickFrame();

    bool BindMultiRangeStorage(const Binding& binding, bool is_written,
                               std::span<const MultiRangeSegment> pool);

    void ResolveMultiRangeStorage(Binding& binding, bool is_written,
                                  std::vector<MultiRangeSegment>& pool);

    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// `keep_page_gpu_data`: download GPU-written bytes sharing the written pages first (false
    /// when the memory is going away).
    void WriteMemory(DAddr device_addr, u64 size, bool keep_page_gpu_data = true);

    void CachedWriteMemory(DAddr device_addr, u64 size);

    /// Cancels pending downloads over the range, so a fence can't write them over newer bytes.
    void ClearDownload(DAddr base_addr, u64 size);

    bool OnCPUWrite(DAddr device_addr, u64 size);

    void DownloadMemory(DAddr device_addr, u64 size);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr device_addr, u64 size);

    /// The core consumed the read traps of these pages for a read the texture cache answered:
    /// the next GPU write here must arm them again (GetFlushArea forgets them otherwise).
    void ForgetReadTraps(DAddr device_addr, u64 size) {
        read_trapped_ranges.Subtract(
            Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE),
            Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE) -
                Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE));
    }

    /// Without downloads at every fence, a guest CPU read is the only way GPU-written buffer
    /// data reaches guest memory, so reads must ask the buffer cache (GetFlushArea).
    bool FlushesOnCpuRead() const {
        return !AsyncBufferDownloads();
    }

    bool InlineMemory(DAddr dest_address, size_t copy_size, std::span<const u8> inlined_buffer);

    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) {
        auto& current = channel_state->uniform_buffers[stage][index];
        const u64 map_serial = gpu_memory->GetMapSerial();
        if (!channel_state->has_deleted_buffers && current.buffer_id &&
            current.buffer_id != NULL_BUFFER_ID && current.gpu_addr == gpu_addr &&
            current.size == size && current.map_serial == map_serial && current.device_addr != 0)
            [[likely]] {
            return;
        }
        const std::optional<DAddr> device_addr =
            (current.gpu_addr == gpu_addr && current.map_serial == map_serial &&
             current.device_addr != 0)
                ? std::optional<DAddr>{current.device_addr}
                : gpu_memory->GpuToCpuAddress(gpu_addr);
        if (!device_addr) {
            current = NULL_BINDING;
            channel_state->unresolved_uniform_buffers[stage] |= 1U << index;
            return;
        }
        if (!channel_state->has_deleted_buffers && current.buffer_id &&
            current.buffer_id != NULL_BUFFER_ID && current.device_addr == *device_addr &&
            current.size == size) {
            current.gpu_addr = gpu_addr;
            current.map_serial = map_serial;
            return;
        }
        current = Binding{
            .device_addr = *device_addr,
            .gpu_addr = gpu_addr,
            .map_serial = map_serial,
            .size = size,
            .buffer_id = BufferId{},
        };
        channel_state->unresolved_uniform_buffers[stage] |= 1U << index;
    }

    void DisableGraphicsUniformBuffer(size_t stage, u32 index) {
        auto& current = channel_state->uniform_buffers[stage][index];
        if (current.device_addr == 0 && current.size == 0 && current.buffer_id == NULL_BUFFER_ID)
            [[likely]] {
            return;
        }
        current = NULL_BINDING;
        channel_state->unresolved_uniform_buffers[stage] &= ~(1U << index);
    }

    void UpdateGraphicsBuffers(bool is_indexed);

    void UpdateComputeBuffers();

    void BindHostGeometryBuffers(bool is_indexed);

    void BindHostStageBuffers(size_t stage);

    void BindHostComputeBuffers();

    void SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
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

    void SetComputeUniformBufferState(u32 mask, const ComputeUniformBufferSizes* sizes) {
        channel_state->enabled_compute_uniform_buffer_mask = mask;
        channel_state->compute_uniform_buffer_sizes = sizes;
    }

    void UnbindGraphicsStorageBuffers(size_t stage) {
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

    bool BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                   bool is_written, u32 indirect_offset = ~0U);

    void UnbindGraphicsTextureBuffers(size_t stage) {
        if (channel_state->enabled_texture_buffers[stage] == 0) {
            return;
        }
        channel_state->enabled_texture_buffers[stage] = 0;
        channel_state->written_texture_buffers[stage] = 0;
        channel_state->image_texture_buffers[stage] = 0;
    }

    void BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                   PixelFormat format, bool is_written, bool is_image);

    void UnbindComputeStorageBuffers() {
        if (channel_state->enabled_compute_storage_buffers == 0 &&
            channel_state->image_compute_texture_buffers == 0) {
            return;
        }
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

    void BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                  bool is_written, u32 indirect_offset = ~0U);

    void UnbindComputeTextureBuffers() {
        if (channel_state->enabled_compute_texture_buffers == 0) {
            return;
        }
        channel_state->enabled_compute_texture_buffers = 0;
        channel_state->written_compute_texture_buffers = 0;
        channel_state->image_compute_texture_buffers = 0;
    }

    void BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size, PixelFormat format,
                                  bool is_written, bool is_image);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                       ObtainBufferSynchronize sync_info,
                                                       ObtainBufferOperation post_op);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainCPUBuffer(DAddr gpu_addr, u32 size,
                                                          ObtainBufferSynchronize sync_info,
                                                          ObtainBufferOperation post_op);
    void FlushCachedWrites();

    /// Where the CPU writes into GPU-written memory that were collected but not applied yet
    /// (WriteMemory) come from: downloads must not write the GPU's older copy over them.
    using PendingCpuWrites = std::function<void(const std::function<void(DAddr, size_t)>&)>;
    void SetPendingCpuWrites(PendingCpuWrites source) {
        pending_cpu_writes = std::move(source);
    }

    /// Takes CPU writes about to be applied one by one out of the GPU-written ranges first, so
    /// applying one doesn't download the GPU's copy over another (WriteMemory keeps the GPU data
    /// on the pages it writes).
    void ClaimCpuWrites(std::span<const std::pair<DAddr, u64>> ranges);

    /// Return true when there are uncommitted buffers to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    void AccumulateFlushes();

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();
    void CommitAsyncFlushesHigh();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();
    void PopAsyncBuffers();

    bool DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount);

    bool DMAClear(GPUVAddr src_address, u64 amount, u32 value);

    /// Switch 2 GPU-written command lists (nx2.h, g_nx2_command_list_pages). While collecting (a
    /// compute dispatch, DMA copy, or draw being recorded), writes to command list pages are noted;
    /// Nx2CopyListPages then records copies of those pages into host memory after the work.
    void Nx2CollectListCopies(bool collect) {
        nx2_collect_list_copies = collect;
    }
    [[nodiscard]] bool HasPendingNx2ListCopies() const noexcept {
        return !nx2_list_copy_requests.empty();
    }
    /// True when it recorded copies, which the GPU completes at `tick` (the caller submits).
    bool Nx2CopyListPages(u64 tick);
    enum class Nx2ListRead { Unknown, Copied, Synced, NeedsSync };
    /// Front thread: Synced when guest memory holds the page `device_addr` lies in (flushed and
    /// written by no GPU work since), else Copied with `out` filled from the copy of its last
    /// write once the GPU is done with it. Unknown otherwise: the caller drains and flushes.
    /// Pages draws write are only Synced or Unknown, and NeedsSync until `past_sync`: the
    /// split's back ran the work before the last host-level wait (GpuSplit::WaitForSync).
    Nx2ListRead Nx2ReadListCopy(DAddr device_addr, std::span<u8> out, bool past_sync);
    /// Front thread, the split drained: the command list page was flushed to guest memory.
    void Nx2NoteListSynced(DAddr page);
    /// CPU and inline writes: they reach guest memory, so a synced page stays synced, but its copy
    /// no longer holds the page unless updated in place.
    void Nx2NoteListHostWrite(DAddr device_addr, u64 size, std::span<const u8> data = {});

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    /// Visits the uniform and storage buffers bound for the last draw or dispatch with the cached
    /// host buffer holding each range, if one does, for the frame dump. Nothing is created or
    /// synchronized. Stage 5 is compute.
    template <typename Func>
    void ForEachBoundBuffer(bool compute, Func&& func) {
        const auto visit = [&](u32 stage, bool storage, u32 index, bool written,
                               const Binding& binding, u32 size) {
            if (binding.device_addr == 0 || size == 0) {
                return;
            }
            const Buffer* host = nullptr;
            u32 offset = 0;
            const u64 page = binding.device_addr >> CACHING_PAGEBITS;
            if (const BufferId id = page < page_table.size() ? page_table[page] : BufferId{}) {
                const Buffer& buffer = slot_buffers[id];
                if (buffer.IsInBounds(binding.device_addr, size)) {
                    host = &buffer;
                    offset = buffer.Offset(binding.device_addr);
                }
            }
            func(stage, storage, index, written, binding, size, host, offset);
        };
        if (compute) {
            const auto* const sizes = channel_state->compute_uniform_buffer_sizes;
            ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
                const Binding& binding = channel_state->compute_uniform_buffers[index];
                visit(5, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
                const Binding& binding = channel_state->compute_storage_buffers[index];
                visit(5, true, index,
                      ((channel_state->written_compute_storage_buffers >> index) & 1) != 0, binding,
                      binding.size);
            });
            return;
        }
        const auto* const sizes = channel_state->uniform_buffer_sizes;
        for (u32 stage = 0; stage < NUM_STAGES; ++stage) {
            ForEachEnabledBit(channel_state->enabled_uniform_buffer_masks[stage], [&](u32 index) {
                const Binding& binding = channel_state->uniform_buffers[stage][index];
                visit(stage, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[stage][index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
                const Binding& binding = channel_state->storage_buffers[stage][index];
                visit(stage, true, index,
                      ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0, binding,
                      binding.size);
            });
        }
    }

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(DAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(DAddr addr, size_t size);

    void SetDrawIndirect(
        const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect_) {
        current_draw_indirect = current_draw_indirect_;
    }

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectCount();

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectBuffer();

    template <typename Func>
    void BufferOperations(Func&& func) {
        do {
            channel_state->has_deleted_buffers = false;
            func();
        } while (channel_state->has_deleted_buffers);
    }

    Common::SpinRecursiveMutex mutex;
    Runtime& runtime;
    bool any_buffer_uploaded = false;
    /// Indirectly read banks of native programs already uploaded this frame.
    std::unordered_set<DAddr> native_indirect_uniforms_this_frame;
    struct NativeUniformBankShadow {
        u64 last_draw_ticket{};
        u64 last_frame_tick{~u64{0}};
        u64 last_skipped_draw_ticket{~u64{0}}; ///< Profiler only: bank_once_per_frame skips
        u32 initialized_mask{};
        std::array<u8, 0x10000> bytes{};
    };
    std::unordered_map<DAddr, NativeUniformBankShadow> native_indirect_uniform_shadow;
    std::array<std::pair<DAddr, NativeUniformBankShadow*>, 8> native_indirect_uniform_cache{};
    DAddr native_indirect_uniform_last_bank = ~DAddr{0};
    NativeUniformBankShadow* native_indirect_uniform_last_ptr = nullptr;
    u64 native_indirect_uniform_draw_ticket = 0;

    struct LastSyncedRange {
        DAddr begin{};
        DAddr end{};
    };
    u64 last_synced_draw_ticket = 0;
    u32 last_synced_count = 0;
    std::array<LastSyncedRange, 8> last_synced_ranges{};
    bool last_sync_deduped = false; ///< The last SynchronizeBuffer returned on last_synced_ranges

    /// Nx2Perf bind_memo: the last full bind of each non-written uniform and storage buffer. A
    /// bind of the same range with the same tracker epochs, scheduler tick and frame skips the
    /// touch, sync and usage mark: the range was clean after the sync and nothing marked it since.
    struct BindMemo {
        const void* channel{};
        BufferId buffer_id{};
        DAddr device_addr{};
        u32 size{};
        bool valid{};
        bool usage_marked{};
        u64 cpu_epoch{};
        u64 gpu_epoch{};
        u64 tick{};
        u64 frame{};
    };
    std::array<std::array<BindMemo, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES> uniform_bind_memo{};
    std::array<std::array<BindMemo, NUM_STORAGE_BUFFERS>, NUM_STAGES> storage_bind_memo{};
    u64 bind_memo_mismatches = 0;

    static constexpr size_t ResolvedStorageAddrCacheSize = 4096;
    struct ResolvedStorageAddrCacheEntry {
        GPUVAddr gpu_addr{};
        GPUVAddr aligned_gpu_addr{};
        DAddr device_addr{};
        u64 map_serial{};
        size_t memory_id{};
        u32 binding_size{};
        u8 flags{};
        bool single_range{};
        bool valid{};
    };
    mutable std::array<ResolvedStorageAddrCacheEntry, ResolvedStorageAddrCacheSize>
        resolved_storage_addr_cache{};

private:
    template <typename Mask, typename Func>
    static void ForEachEnabledBit(Mask enabled_mask, Func&& func) {
        for (u32 index = 0; enabled_mask != 0; ++index, enabled_mask >>= 1) {
            const int disabled_bits = std::countr_zero(enabled_mask);
            index += disabled_bits;
            enabled_mask >>= disabled_bits;
            func(index);
        }
    }

    template <typename Func>
    void ForEachBufferInRange(DAddr device_addr, u64 size, Func&& func) {
        const u64 page_end = Common::DivCeil(device_addr + size, CACHING_PAGESIZE);
        for (u64 page = device_addr >> CACHING_PAGEBITS; page < page_end;) {
            const BufferId buffer_id = page_table[page];
            if (!buffer_id) {
                ++page;
                continue;
            }
            Buffer& buffer = slot_buffers[buffer_id];
            func(buffer_id, buffer);

            const DAddr end_addr = buffer.CpuAddr() + buffer.SizeBytes();
            page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
        }
    }

    static bool IsRangeGranular(DAddr device_addr, size_t size) {
        return (device_addr & ~Core::DEVICE_PAGEMASK) ==
               ((device_addr + size) & ~Core::DEVICE_PAGEMASK);
    }

    void RunGarbageCollector(bool aggressive_gc);

    void BindHostIndexBuffer();

    void BindHostVertexBuffers();

    void BindHostDrawIndirectBuffers();

    void BindHostGraphicsUniformBuffers(size_t stage);

    void BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index, bool needs_bind);

    void BindHostGraphicsStorageBuffers(size_t stage);

    void BindHostGraphicsTextureBuffers(size_t stage);

    void BindHostTransformFeedbackBuffers();

    void BindHostVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);

    void BindHostComputeUniformBuffers();

    void BindHostComputeStorageBuffers();

    void BindHostComputeTextureBuffers();

    void DoUpdateGraphicsBuffers(bool is_indexed);

    void DoUpdateComputeBuffers();

    void UpdateIndexBuffer();

    void UpdateVertexBuffers();

    void UpdateVertexBuffer(u32 index);

    void UpdateDrawIndirect();

    void UpdateUniformBuffers(size_t stage);

    void UpdateStorageBuffers(size_t stage);

    void UpdateTextureBuffers(size_t stage);

    void UpdateTransformFeedbackBuffers();

    void UpdateTransformFeedbackBuffer(u32 index);

    void UpdateComputeUniformBuffers();

    void UpdateComputeStorageBuffers();

    void UpdateComputeTextureBuffers();

    void MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size,
                           bool needs_sync = false);

    /// Counts a GPU write to the command list pages in the range (invalidating their copies) and,
    /// while collecting, asks for new copies.
    void Nx2NoteListWrite(BufferId buffer_id, DAddr device_addr, u64 size);

    [[nodiscard]] BufferId FindBuffer(DAddr device_addr, u32 size, bool sparse_compatible);

    void SynchronizeBufferWrites(Buffer& buffer);

    [[nodiscard]] OverlapResult ResolveOverlaps(DAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    [[nodiscard]] BufferId CreateBuffer(DAddr device_addr, u32 wanted_size, bool sparse_compatible);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    void TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept {
        if (buffer_id != NULL_BUFFER_ID) {
            lru_cache.Touch(buffer.getLRUID(), frame_tick);
        }
    }

    bool SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size);

    [[nodiscard]] static bool BindMemoEnabled();
    [[nodiscard]] bool BindMemoHit(const BindMemo& memo, Buffer& buffer, BufferId buffer_id,
                                   DAddr device_addr, u32 size, u32 offset, const char* kind,
                                   size_t stage, u32 index);
    void BindMemoNote(BindMemo& memo, BufferId buffer_id, DAddr device_addr, u32 size,
                      u64 cpu_epoch_before);
    void BindMemoNoteUsage(BindMemo& memo);

    void UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                      std::span<BufferCopy> copies);

    void ImmediateUploadMemory(Buffer& buffer, u64 largest_copy,
                               std::span<const BufferCopy> copies);

    void MappedUploadMemory(Buffer& buffer, u64 total_size_bytes, std::span<BufferCopy> copies);

    void DownloadBufferMemory(Buffer& buffer_id);

    void DownloadBufferMemory(Buffer& buffer_id, DAddr device_addr, u64 size);

    /// Downloads the GPU-written data of several buffers with one wait for the GPU.
    void DownloadBuffersMemory(std::span<const BufferId> buffer_ids);

    void DownloadRangesMemory(std::span<const std::pair<DAddr, u64>> ranges);

    void DeleteBuffer(BufferId buffer_id, bool do_not_mark = false);

    [[nodiscard]] Binding StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index, bool is_written,
                                               bool native_pointer = false,
                                               bool table_pointer = false) const;

    [[nodiscard]] TextureBufferBinding GetTextureBufferBinding(GPUVAddr gpu_addr, u32 size,
                                                               PixelFormat format);

    [[nodiscard]] std::span<const u8> ImmediateBufferWithData(DAddr device_addr, size_t size);

    [[nodiscard]] std::span<u8> ImmediateBuffer(size_t wanted_capacity);

    [[nodiscard]] bool HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept;

    [[nodiscard]] Binding& VertexBufferSlot(u32 index);

    [[nodiscard]] const Binding& VertexBufferSlot(u32 index) const;

    void UpdateVertexBufferSlot(u32 index, const Binding& binding);

    /// Flags the vertex buffers overlapping a written range so host copies derived from them are
    /// rebuilt.
    void MarkVertexBuffersStale(DAddr device_addr, u64 size);

    /// Re-dirties repacked vertex buffers whose source data was written since they were bound.
    void RefreshRepackedVertexBuffers();

    /// CPU writes collected but not yet applied (SetPendingCpuWrites).
    [[nodiscard]] Common::RangeSet<DAddr> PendingCpuWriteRanges();

    /// Writes downloaded bytes to guest memory, except over `cpu_written`.
    void WriteDownloadedMemory(const Common::RangeSet<DAddr>& cpu_written, DAddr device_addr,
                               const u8* data, u64 size);
    void InlineMemoryImplementation(DAddr dest_address, size_t copy_size,
                                    std::span<const u8> inlined_buffer);

    Tegra::MaxwellDeviceMemoryManager& device_memory;

    Common::SlotVector<Buffer> slot_buffers;
#ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
#else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Buffer, TICKS_TO_DESTROY> delayed_destruction_ring;

    const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect{};

    u32 last_index_count = 0;

    u32 enabled_vertex_buffers_mask = 0;
    u32 stale_vertex_buffers_mask = 0;
    u64 vertex_buffers_serial = 0;
    std::array<Binding, 32> v_buffer{};

    boost::container::small_vector<BufferCopy, 4> upload_copies;

    MemoryTracker memory_tracker;
    Common::RangeSet<DAddr> uncommitted_gpu_modified_ranges;

    /// Whether GPU-written buffer ranges are copied back to guest memory at the next fence;
    /// otherwise only when something flushes them (as upstream yuzu below High GPU accuracy).
    /// Not for Switch 2 titles: title 1 had 250-330 MB/s copied back (a fence per compute
    /// dispatch, the fencing thread holding the buffer cache while it wrote them), while its CPU
    /// read GPU-written memory at most 64 times in 200 s (counted with reactive flushing) and
    /// the native MME's DMA reads never touched those pages. Without them a fence costs ~1 us
    /// instead of ~5 and draw setup ~7% less. EDEN_NX2_BUFFER_DOWNLOADS=0/1 forces it.
    bool AsyncBufferDownloads() const {
        static const int forced = [] {
            const char* value = std::getenv("EDEN_NX2_BUFFER_DOWNLOADS");
            return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
        }();
        if (forced >= 0) {
            return forced == 1;
        }
        return gpu_memory == nullptr || !gpu_memory->UsesNativeTiling();
    }

    /// Whether a GPU write to this range is copied back at the next fence. Ranges the guest CPU
    /// has read (Preflushable, set by the first reactive flush) always are: later reads skip the
    /// flush and rely on these downloads, e.g. collision meshes skinned by compute shaders.
    bool DownloadsAtFence(DAddr device_addr, u64 size) noexcept {
        return AsyncBufferDownloads() || memory_tracker.IsRegionPreflushable(device_addr, size);
    }

    /// Whether the range holds GPU-written data that no fence downloads (not Preflushable).
    bool HasGpuWritesNotDownloadedAtFence(DAddr device_addr, u64 size) {
        if (AsyncBufferDownloads()) {
            return false;
        }
        bool found = false;
        memory_tracker.ForEachDownloadRange(
            device_addr, size, false, [&](u64 start, u64 range_size) {
                if (found) {
                    return;
                }
                u64 covered = 0;
                memory_tracker.ForEachPreflushableRange(
                    start, range_size, [&](u64 lower, u64 length) {
                        const u64 begin = (std::max)(lower, start);
                        const u64 end = (std::min)(lower + length, start + range_size);
                        covered += end > begin ? end - begin : 0;
                    });
                found = covered < range_size;
            });
        return found;
    }

    /// A GPU write: queues its Preflushable parts for download at the next fence and traps reads
    /// of the rest. Queuing the whole range when any page of it was Preflushable copied whole
    /// written storage buffers (up to 8 MiB) back at every fence: ~5 GB/s in title 1's forest.
    void QueueGpuWrite(DAddr device_addr, u64 size) {
        if (AsyncBufferDownloads()) {
            uncommitted_gpu_modified_ranges.Add(device_addr, size);
            return;
        }
        const DAddr end = device_addr + size;
        DAddr cursor = device_addr;
        memory_tracker.ForEachPreflushableRange(device_addr, size, [&](DAddr start, u64 range_size) {
            const DAddr lower = (std::max)(start, cursor);
            const DAddr upper = (std::min)(start + range_size, end);
            if (lower >= upper) {
                return;
            }
            if (lower > cursor) {
                ArmReadTraps(cursor, lower - cursor);
            }
            // PreflushAging observes reads of marked ranges: GetFlushArea finds them marked and
            // lets the read go on without a flush, but stamps the blocks read.
            if (Nx2Perf::On(Nx2Perf::Opt::PreflushAging)) {
                ArmReadTraps(lower, upper - lower);
            }
            uncommitted_gpu_modified_ranges.Add(lower, upper - lower);
            cursor = upper;
        });
        if (cursor < end) {
            ArmReadTraps(cursor, end - cursor);
        }
    }

    /// GPU writes that are not downloaded at fences: trap the guest CPU's next read of their
    /// pages, which flushes them on demand (or marks them Preflushable with preflushing enabled).
    void ArmReadTraps(DAddr device_addr, u64 size) {
        const DAddr begin = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
        const DAddr end = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
        // Only the parts not armed yet: the core visits each page it is given, and a buffer the
        // GPU rewrites every frame is mostly still armed (the guest read a few of its pages).
        read_trapped_ranges.Arm(begin, end - begin, [this](DAddr part, u64 part_size) {
            device_memory.ArmReadTraps(part, part_size);
        });
    }

    /// Fence-time downloads: the range of a buffer (src_offset) and its place in staging.
    using Nx2Downloads = boost::container::small_vector<std::pair<BufferCopy, BufferId>, 16>;
    /// Opt::DownloadBatch: `downloads` per buffer with nearby ranges merged into `batched`
    /// (staging offsets from 0); returns the staging size.
    u64 Nx2BatchDownloads(const Nx2Downloads& downloads, Nx2Downloads& batched);
    /// PERF_VERIFY: whether `batched` copies every byte of `downloads` and writes back the same.
    bool Nx2VerifyDownloadBatch(const Nx2Downloads& downloads, const Nx2Downloads& batched);
    /// Profiling: counts a download's bytes by the age of their blocks' preflush stamps.
    void Nx2CountDownloadAges(DAddr device_addr, u64 size);
    /// The guest CPU read [begin, end): `marked` when GetFlushArea marked it Preflushable after a
    /// flush, else (PreflushAging) a trapped read of a marked range.
    void Nx2StampPreflushBlocks(DAddr begin, DAddr end, bool marked);
    /// PreflushAging, every 60 frames: drops the marks of blocks the CPU stopped reading.
    void Nx2AgePreflushBlocks();

    static constexpr u64 NX2_PREFLUSH_BLOCK_BITS = 16; ///< 64 KiB blocks
    static constexpr u32 NX2_PREFLUSH_MIN_LIMIT = 120;  ///< Frames unread before a drop
    static constexpr u32 NX2_PREFLUSH_MAX_LIMIT = 3600;
    /// A 64 KiB block with Preflushable marks. Without PreflushAging reads of marked ranges are
    /// not trapped, so `last_read_frame` is when the block was first marked. Guarded by `mutex`.
    struct Nx2PreflushBlock {
        u64 last_read_frame{};
        u64 limit_frame{};  ///< Dropped: start of the decay period (the drop or the last halving)
        u32 limit_frames{NX2_PREFLUSH_MIN_LIMIT};
        bool dropped{};     ///< Marks dropped (PreflushAging) and not marked again since
    };
    std::unordered_map<u64, Nx2PreflushBlock> nx2_preflush_blocks;
    u64 nx2_preflush_drops{};   ///< Blocks dropped since start
    u64 nx2_preflush_remarks{}; ///< Reads of dropped blocks that flushed (stalled) and marked them

    ReadTrapTracker read_trapped_ranges;
    /// Ranges the guest CPU read after GPU writes. A read that must flush downloads the GPU
    /// writes in all of them, so the reads that usually follow it find nothing left to flush.
    Common::RangeSet<DAddr> cpu_read_ranges;
    bool download_cpu_read_ranges{};
    Common::RangeSet<DAddr> gpu_modified_ranges;
    std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges;

    // Async Buffers
    std::deque<std::optional<Async_Buffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy, 4>> pending_downloads;
    /// For each entry of pending_downloads: the ranges it still writes to guest memory when its
    /// fence pops. A write to a range after a download was taken removes it from that download
    /// (and every other pending one): its bytes are older than the write.
    std::deque<Common::RangeSet<DAddr>> pending_download_ranges;
    PendingCpuWrites pending_cpu_writes;
    std::optional<Async_Buffer> current_buffer;

    std::deque<Async_Buffer> async_buffers_death_ring;

    size_t immediate_buffer_capacity = 0;
    Common::ScratchBuffer<u8> immediate_buffer_alloc;
    /// Guest memory large uploads came from since the last cold hint (device address, size).
    std::vector<std::pair<DAddr, u64>> uploaded_buffer_ranges;

    struct LRUItemParams {
        using ObjectType = BufferId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;
    VirtualRangeCache virtual_ranges;
    std::vector<MultiRangeSegment> graphics_segments;
    std::vector<MultiRangeSegment> compute_segments;
    u64 frame_tick = 0;
    u64 total_used_memory = 0;
    u64 buffer_bytes = 0; ///< The buffers' own size (total_used_memory may be the device's)
    u64 minimum_memory = 0;
    u64 critical_memory = 0;
    BufferId inline_buffer_id;
#ifdef YUZU_LEGACY
    bool immediately_free = false;
#endif

    // Covers the device address space (MaxwellDeviceTraits::device_virtual_bits).
    std::array<BufferId, ((1ULL << 36) >> CACHING_PAGEBITS)> page_table;
    Common::ScratchBuffer<u8> tmp_buffer;

    /// A command list page (Nx2CopyListPages): the GPU writes noted to it and the last copy.
    using Nx2Staging = decltype(std::declval<Runtime&>().DownloadStagingBuffer(size_t{}, true));
    struct Nx2ListPage {
        u64 writes{};      ///< GPU writes noted to the page
        u64 copy_writes{}; ///< `writes` when the copy was made: the copy is current while equal
        u64 copy_tick{};   ///< GPU tick that completes the copy
        u64 copy_time{};   ///< Nx2Profile::Now when the copy was recorded (0 unless profiling)
        /// `writes` when the front last flushed the page: guest memory holds it while equal
        u64 synced_writes{~u64{0}};
        u32 copy_begin{}; ///< Bytes of the page the copy holds (a buffer can end inside it)
        u32 copy_end{};
        /// Written outside a compute dispatch or a DMA copy (a draw's storage writes) at least
        /// once: the front can't tell from what it sent which draws write the page, so it reads
        /// no copy. A draw's writes reach a pushbuffer fetch only after a host-level wait, so
        /// the page's lists read guest memory once the back ran the work before the last such
        /// wait and no write came since the front flushed the page (Nx2ReadListCopy).
        bool draw_written{};
        std::optional<Nx2Staging> copy;
    };
    std::mutex nx2_list_mutex; ///< nx2_list_pages: the split's front reads the copies
    std::unordered_map<DAddr, Nx2ListPage> nx2_list_pages;
    /// Copies replaced while the GPU may still write them: freed once it is past their tick.
    std::vector<std::pair<u64, Nx2Staging>> nx2_retired_list_copies;
    boost::container::small_vector<std::pair<BufferId, DAddr>, 8> nx2_list_copy_requests;
    bool nx2_collect_list_copies{};
    int nx2_graphics_stage{-1};
};

} // namespace VideoCommon
