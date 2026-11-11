// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct ComputePipelineVariant {
    std::unique_ptr<ComputePipeline> pipeline;
    std::vector<std::pair<u32, u32>> null_pointer_slots;
    std::vector<u64> mapped_pointers;
};

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    /// Replacement pipeline from a stale-slot rebuild, kept here until its worker compile finishes
    std::unique_ptr<ComputePipeline> pending;
    /// Extra persisted variants with distinct MappedStoragePointerKeys built at boot
    std::vector<ComputePipelineVariant> alternates;
    std::string failure;
    /// Constant buffer slots that were null pointers at translation (see NullStoragePointerSlots)
    std::vector<std::pair<u32, u32>> null_pointer_slots;
    /// Constant buffer slots bound as storage buffer pointers (see MappedStoragePointerKeys)
    std::vector<u64> mapped_pointers;
    u32 rebuilds{};
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline(bool is_indexed = false);
    [[nodiscard]] bool IsIndirectResetShader() const;
    /// Nx2Perf MacroReadWriterSet: the program epoch the current programs were resolved in
    /// (0: none) and their device code ranges.
    [[nodiscard]] u64 Nx2ResolvedEpoch() const noexcept {
        return nx2_resolved_epoch;
    }
    [[nodiscard]] u64 Nx2ResolvedMap() const noexcept {
        return nx2_resolved_map;
    }
    [[nodiscard]] const auto& Nx2CodeRanges() const noexcept {
        return nx2_code_ranges;
    }

    /// Why the last CurrentGraphicsPipeline() returned null (diagnostics).
    [[nodiscard]] const char* GraphicsMissReason() const noexcept {
        return graphics_miss;
    }

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64& native_key);

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath(bool is_indexed);

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline,
                                                  bool is_indexed) noexcept;

    void RecordSkippedGeometry(bool is_indexed, const GraphicsPipeline* pipeline) noexcept;
    [[nodiscard]] bool ShouldSkipDepthOnlyGeometry(bool is_indexed) noexcept;
    void ClearSkippedGeometryIfDrawn(bool is_indexed) noexcept;

    void DrainReadyDiskPipelines();
    [[nodiscard]] bool CanSkipForDiskReplay() const noexcept;

    /// Nx2Perf shader_build_budget: whether a new graphics pipeline is left unbuilt this frame.
    [[nodiscard]] bool DeferRuntimeBuild();
    void ChargeRuntimeBuild(std::chrono::steady_clock::time_point begin);

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, bool native_turing = false);

    [[nodiscard]] bool StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const;

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers)
    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                    const ShaderInfo* shader,
                                                    std::span<const u64> keep_pointers = {});

    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                    const ComputePipelineCacheKey& key,
                                                    Shader::Environment& env,
                                                    PipelineStatistics* statistics,
                                                    bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};
    mutable const char* graphics_miss{"no-pipeline"};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since
    std::unordered_set<u64> evicted_keys; ///< Key hashes of every evicted pipeline, to count rebuilds

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    /// Replaced compute pipelines, destroyed once the GPU and their queued build are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    static constexpr size_t ComputeFastCacheSize = 64;
    struct ComputeFastCacheEntry {
        ComputePipelineCacheKey key{};
        ComputePipeline* pipeline{};
    };
    std::array<ComputeFastCacheEntry, ComputeFastCacheSize> compute_fast_cache{};

    /// Nx2Perf pipeline_fast_cache: filled only by the slow path, validated by the full key and
    /// cleared whenever pipelines are evicted.
    static constexpr size_t GraphicsFastCacheSize = 256;
    std::array<GraphicsPipeline*, GraphicsFastCacheSize> graphics_fast_cache{};
    u64 graphics_fast_mismatches{};

    ShaderPools main_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;
    /// Task and mesh pipelines, with the mesh state they are translated with.
    std::filesystem::path mesh_pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    u64 build_budget_frame{~u64{}};
    std::chrono::steady_clock::duration build_budget_spent{};
    std::chrono::steady_clock::duration build_budget{};
    u64 deferred_builds{};
    u64 over_budget_small{};
    u64 over_budget_mesh{};
    void SaveUsedPipelines();

    static constexpr size_t SkippedGeometryTableSize = 256;
    struct SkippedGeometryEntry {
        u64 key{};
        u64 frame{};
        const GraphicsPipeline* pipeline{};
    };
    std::array<SkippedGeometryEntry, SkippedGeometryTableSize> skipped_geometry{};
    u64 last_skipped_geometry_frame{~u64{}};

    static constexpr size_t DeferredCandidateSlots = 64;
    struct DeferredCandidate {
        u64 key_hash{};
        u32 draws{};
        bool is_gbuffer{};
    };
    std::array<DeferredCandidate, DeferredCandidateSlots> deferred_candidates{};
    size_t deferred_candidate_count{};
    static constexpr size_t PriorityBuildKeySlots = 16;
    std::array<u64, PriorityBuildKeySlots> priority_build_keys{};
    size_t priority_build_key_count{};

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    ::Common::unordered_map<GraphicsPipelineCacheKey, std::vector<VideoCommon::FileEnvironment>>
        skipped_disk_envs;
    std::unordered_set<u64> in_flight_disk_replays;
    std::mutex ready_disk_pipelines_mutex;
    std::vector<std::pair<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>>>
        ready_disk_pipelines;
    std::atomic_bool ready_disk_pipelines_pending{};
    u64 disk_replayed_count{};

    Common::ThreadWorker workers;
    Common::ThreadWorker disk_replay_workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;

    /// Cached graphics pipelines boot skipped (not in used_pipelines) are built on background
    /// threads once the game runs, so a draw that needs one finds it built instead of translating
    /// it on the GPU thread. EDEN_NX2_BACKGROUND_PIPELINES=0 turns it off.
    std::unique_ptr<GraphicsPipeline> TakeBackgroundPipeline(const GraphicsPipelineCacheKey& key);
    void StopBackgroundPipelines();
    std::mutex background_mutex;
    /// Built and not yet asked for by a draw.
    std::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> background_ready;
    /// Keys the GPU thread built itself meanwhile: their background build is skipped or dropped.
    std::unordered_set<GraphicsPipelineCacheKey> background_claimed;
    std::atomic_bool background_active{};
    std::atomic_bool background_stop{};
    std::atomic<size_t> background_left{};
    size_t background_taken{}; ///< Under background_mutex
    std::chrono::steady_clock::time_point background_start{};
    /// Last: destroyed (its threads joined) before the state above.
    std::unique_ptr<Common::ThreadWorker> background_workers;
};

} // namespace Vulkan
