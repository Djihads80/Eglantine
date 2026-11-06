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
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
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

using VideoCommon::GraphicsEnvironment;
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

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();
    [[nodiscard]] bool IsIndirectResetShader() const;

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64& native_key);

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    /// Whether the current draw may be skipped while its pipeline is being built.
    [[nodiscard]] bool CanSkipDraw() const noexcept;

    /// Builds the pipeline of graphics_key, which is not in graphics_cache. Returns false when it
    /// was left translating on a worker thread (the draw is skipped), true when graphics_cache
    /// now holds it (null if the build failed).
    bool BuildNewGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    // Asynchronous translation of native graphics pipelines (NX2_SHADER_STUTTER.md).
    using GraphicsEnvironmentArray = std::array<GraphicsEnvironment, Maxwell::MaxShaderProgram>;

    /// A graphics pipeline translated on a worker thread, waiting for the GPU thread.
    struct TranslatedGraphicsPipeline {
        GraphicsPipelineCacheKey key;
        std::unique_ptr<GraphicsPipeline> pipeline; ///< Null when the build failed.
        GraphicsEnvironmentArray envs;              ///< For the disk cache.
        std::chrono::steady_clock::duration build_time{};
    };

    [[nodiscard]] bool CanTranslateAsynchronously() const;

    /// Queues the translation of graphics_key's pipeline on a worker thread. Returns a future
    /// that becomes ready when the result is waiting to be adopted, or nothing when the
    /// environments could not be detached from the engine.
    std::optional<std::shared_future<void>> QueueGraphicsTranslation();

    /// Waits up to the wait budget for a queued translation. True if it was adopted in time.
    bool WaitForGraphicsTranslation(std::shared_future<void> done);

    /// Runs on a translation worker thread.
    void TranslateGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs, std::promise<void> done);

    /// Moves the pipelines finished by the workers into graphics_cache (GPU thread).
    void AdoptTranslatedGraphicsPipelines();

    void SerializeGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs);

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                           const ShaderInfo* shader);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(ShaderPools& pools,
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
    bool use_async_translation{};
    std::chrono::microseconds translation_wait_budget{};
    size_t num_translation_threads{};

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since

    ::Common::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>> compute_cache;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;

    /// Cached graphics pipelines boot skipped (not in used_pipelines) are built on background
    /// threads once the game runs, so a draw that needs one finds it built instead of translating
    /// it on the GPU thread. EDEN_NX2_BACKGROUND_PIPELINES (off by default on Android).
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
    /// Destroyed (its threads joined) before the state above; ~PipelineCache stops it first.
    std::unique_ptr<Common::ThreadWorker> background_workers;
    /// Keys whose pipeline is being translated on a worker (GPU thread only).
    /// Keys whose translation runs on a worker, with what to wait on for them.
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::shared_future<void>> translating_graphics;
    /// Finished translations, handed from the workers to the GPU thread.
    std::mutex translated_mutex;
    std::vector<TranslatedGraphicsPipeline> translated_graphics;
    std::atomic<size_t> num_translated{};
    /// Declared last so it is destroyed first: its threads finish the translation they are
    /// running (and drop the queued ones) while everything they use is still alive.
    Common::ThreadWorker translation_workers;
};

} // namespace Vulkan
