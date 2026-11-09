// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <queue>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "video_core/gpu_split.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCommon {
template <typename Trait>
class QueryCacheBase;
}

namespace Vulkan {

class CommandPool;
class Device;
class Framebuffer;
class GraphicsPipeline;
class StateTracker;

struct QueryCacheParams;

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:
    explicit Scheduler(const Device& device, StateTracker& state_tracker);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    u64 Flush(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {});

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {});

    /// On the split GPU thread, a fence or a command list copy submits only if the last
    /// submission is older than a window (EDEN_NX2_FENCE_FLUSH_US, default 500 us, 0 submits at
    /// once); otherwise it goes with the next one. The back submits deferred work before it
    /// sleeps and when another thread asks (WaitSubmitted). True when the flush was deferred.
    bool DeferFlush();
    /// Submits deferred work (the split's back, from its idle hook).
    void FlushDeferred();
    /// Threads other than the split's back: waits until tick is submitted, asking the back to
    /// submit deferred work (logged every 2 s while it waits). False only when the back thread
    /// is not running, so nothing will submit it: the caller then submits it itself.
    bool WaitSubmitted(u64 tick);
    /// Allows or stops DeferFlush (returns the previous state).
    bool AllowDeferFlush(bool allow) {
        return std::exchange(defer_flush_allowed, allow);
    }
    /// Whether everything up to tick has been handed to the worker for submission.
    [[nodiscard]] bool IsSubmitted(u64 tick) const noexcept {
        return tick < master_semaphore->CurrentTick();
    }

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    /// Requests to begin a renderpass.
    void RequestRenderpass(const Framebuffer* framebuffer);

    /// Defers a full-attachment color clear so it becomes the next render pass.
    bool DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot, const VkClearValue& value);

    /// Defers a full depth/stencil clear so it becomes the next render pass.
    bool DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value);

    /// Realizes any pending deferred clear before its framebuffer can be moved or freed.
    void FlushDeferredClear();

    /// Requests the current execution context to be able to execute operations only allowed outside
    /// of a renderpass.
    void RequestOutsideRenderPassOperationContext();

    /// Render passes begin lazily: until their first command is handed to the worker, work can go
    /// before them (RecordBeforeRenderPass) without ending them. For emulated task/mesh draws.
    void EnableRenderPassHoisting(bool enable) noexcept {
        hoisting = enable;
    }

    /// Records `command`, an operation allowed only outside a render pass, before the render pass
    /// of `framebuffer` the next commands draw in: ahead of its begin while that is not recorded
    /// yet (hoisting), otherwise the pass ends and begins again after it.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void RecordBeforeRenderPass(const Framebuffer* framebuffer, T&& c) {
        if (hoisting) {
            PrepareBeforeRenderPass(framebuffer, 1024);
            RecordBeforeRenderPass(PRE_PHASES - 1, std::forward<T>(c));
            return;
        }
        RequestRenderpass(framebuffer);
        EndRenderPass();
        Record(std::forward<T>(c));
        RequestRenderpass(framebuffer);
    }

    /// With hoisting, the work ahead of a pending begin runs in phases: a phase's commands run
    /// after every command of the phases before it, so the draws of one pass order their stages
    /// with one barrier each (recorded by the phase's first command).
    static constexpr size_t PRE_PHASES = 4;

    [[nodiscard]] bool HoistsRenderPasses() const noexcept {
        return hoisting;
    }

    /// Makes the render pass of `framebuffer` take work ahead of its begin, `bytes` in each phase
    /// (hoisting): ends it and begins it again lazily when its begin was handed over already, or
    /// a phase lacks the room. Returns which phases hold nothing yet.
    std::array<bool, PRE_PHASES> PrepareBeforeRenderPass(const Framebuffer* framebuffer,
                                                         size_t bytes);

    /// Records `command` in `phase` of the work ahead of the pending begin
    /// (PrepareBeforeRenderPass made room for it).
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void RecordBeforeRenderPass(size_t phase, T&& c) {
        auto command = [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
            command(cmdbuf);
        };
        std::unique_ptr<CommandChunk>& target = pre_chunks[phase];
        if (!target) {
            target = TakeChunk();
        }
        const bool recorded = target->Record(command);
        ASSERT(recorded);
    }

    /// Returns true when a render pass is currently active in the scheduler state.
    bool IsRenderPassActive() const {
        return state.renderpass != VK_NULL_HANDLE;
    }

    /// A guest fragment barrier inside the active render pass: the pass ends at the next draw that
    /// reads what the pass wrote (RenderPassHasStores, a sampled render target), not here.
    void DeferFragmentBarrier() {
        fragment_barrier_pending = state.renderpass != VK_NULL_HANDLE;
    }

    /// Whether a deferred fragment barrier waits for the end of the active render pass. Checking
    /// does not consume it: only EndRenderPass, which records the barrier, clears it.
    [[nodiscard]] bool HasFragmentBarrier() const {
        return fragment_barrier_pending;
    }

    /// A draw in the active render pass writes storage memory or transform feedback.
    void NoteRenderPassStores() {
        render_pass_has_stores = true;
    }

    [[nodiscard]] bool RenderPassHasStores() const {
        return render_pass_has_stores;
    }

    /// Update the pipeline to the current execution context.
    bool UpdateGraphicsPipeline(GraphicsPipeline* pipeline);

    /// Worker thread only: whether the bound graphics pipeline failed to build, so the draws
    /// recorded after it must be dropped instead of reaching the driver without a pipeline.
    void SetGraphicsPipelineMissing(bool missing) {
        graphics_pipeline_missing = missing;
    }
    [[nodiscard]] bool GraphicsPipelineMissing() const {
        return graphics_pipeline_missing;
    }

    /// Update the rescaling state. Returns true if the state has to be updated.
    bool UpdateRescaling(bool is_rescaling);

    /// The next graphics pipeline pushes its down factor again (a pipeline with another push
    /// constant layout was bound).
    void InvalidateRescaling() noexcept {
        state.rescaling_defined = false;
    }

    /// Returns true when the descriptor buffer chunk has to be bound into the command buffer.
    bool UpdateDescriptorBufferChunk(u32 descriptor_chunk);

    /// Invalidates current command buffer state except for render passes
    void InvalidateState();

    /// Assigns the query cache.
    void SetQueryCache(VideoCommon::QueryCacheBase<QueryCacheParams>& query_cache_) {
        query_cache = &query_cache_;
    }

    /// Called after a render pass's end is recorded, outside it (diagnostic breadcrumbs).
    void SetRenderPassEndHook(std::function<void()>&& func) {
        on_render_pass_end = std::move(func);
    }

    // Registers a callback to perform on queue submission.
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// EDEN_NX2_HANG_WATCH: notes the pipeline of the draw or dispatch being recorded, for the
    /// report of a GPU wait that stalls (MasterSemaphore::on_stall). A flag check when off.
    void HangWatchGraphics(const std::array<u64, 6>& hashes) {
        if (hang_watch) [[unlikely]] {
            HangWatchRecordGraphics(hashes);
        }
    }
    void HangWatchCompute(u64 hash, u64 native_key) {
        if (hang_watch) [[unlikely]] {
            HangWatchRecordCompute(hash, native_key);
        }
    }

    /// Send work to a separate thread.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer, vk::CommandBuffer>
    void RecordWithUploadBuffer(T&& command) {
        if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
            VideoCommon::GpuSplit::CheckOwner("Scheduler::Record");
        }
        if (chunk->Record(command)) {
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void Record(T&& c) {
        this->RecordWithUploadBuffer(
            [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
                command(cmdbuf);
            });
    }

    /// Whether the recording thread submitted `tick` within 20 ms.
    [[nodiscard]] bool WaitForSubmission(u64 tick) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
        while (tick >= master_semaphore->CurrentTick()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
        return true;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    /// Bumped when recorded binds are no longer in the current command buffer.
    [[nodiscard]] u64 RecordingEpoch() const noexcept {
        return recording_epoch;
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Waits for the given GPU tick, optionally pacing frames. Threads other than the one that
    /// records (the async present thread) pass may_flush = false: flushing from there records
    /// into the current chunk while that thread does (corrupt command buffers, crashes). They
    /// wait for the recording thread to submit the tick instead, and give up after 20 ms (it may
    /// be waiting for them).
    void Wait(u64 tick, double target_fps = 0.0, bool may_flush = true) {
        if (tick > 0 && (may_flush || WaitForSubmission(tick))) {
            if (tick >= master_semaphore->CurrentTick()) {
                Flush();
            }
            master_semaphore->Wait(tick);
        }
        if (Settings::values.use_speed_limit.GetValue() && target_fps > 0.0) {
            auto now = std::chrono::steady_clock::now();
            if (last_target_fps != target_fps) {
                frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / target_fps));
                max_frame_count = static_cast<int>(0.1 * target_fps);
                last_target_fps = target_fps;
                frame_counter = 0;
                start_time = now;
            }
            frame_counter++;
            auto target_time = start_time + frame_interval * frame_counter;
            if (target_time >= now) {
                constexpr auto spin_tail = std::chrono::milliseconds(1);
                auto sleep_time = target_time - now;
                if (sleep_time > spin_tail * 2) {
                    std::this_thread::sleep_for(sleep_time - spin_tail);
                }
                while (std::chrono::steady_clock::now() < target_time) {
                    std::this_thread::yield();
                }
            } else if (frame_counter > max_frame_count) {
                frame_counter = 0;
                start_time = now;
            }
        }
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() const noexcept {
        return *master_semaphore;
    }

    /// Whether a fence's submission waits for the idle hook (DeferFlush; racy, for reports).
    [[nodiscard]] bool Nx2FlushPending() const noexcept {
        return flush_pending;
    }

    std::mutex submit_mutex;

private:
    std::chrono::steady_clock::time_point last_flush{};
    bool flush_pending{};
    bool defer_flush_allowed{true};

    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const override {

            command(cmdbuf, upload_cmdbuf);
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf);

        template <typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return command_offset == 0;
        }

        bool HasRoom(size_t bytes) const {
            return command_offset + bytes <= sizeof(data);
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data{};
    };

    struct State {
        VkRenderPass renderpass{};
        VkFramebuffer framebuffer{};
        VkExtent2D render_area = {0, 0};
        GraphicsPipeline* graphics_pipeline = nullptr;
        bool is_rescaling = false;
        bool rescaling_defined = false;
        bool needs_state_enable_refresh = false;
        u32 descriptor_buffer_chunk = 0;
        bool descriptor_buffer_bound = false;
    };

    struct DeferredClear {
        const Framebuffer* framebuffer = nullptr;
        u32 color_clear_mask = 0;
        std::array<VkClearValue, 8> color_values{};
        bool depth_stencil = false;
        VkClearValue depth_stencil_value{};
    };

    /// Begins a render pass for the given framebuffer, optionally with clear values.
    void BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                             const VkClearValue* clear_values, u32 clear_value_count);

    /// Records the begin of the current render pass (lazy with hoisting).
    void RecordRenderPassBegin();

    /// Hands the main stream, the work recorded before the pending render pass, its begin and
    /// its commands to the worker, in that order.
    void CommitRenderPassBegin();

    /// A chunk from the reserve.
    std::unique_ptr<CommandChunk> TakeChunk();

    /// Queues a chunk for the worker (an empty one goes back to the reserve).
    void PushChunk(std::unique_ptr<CommandChunk> work);

    /// If a deferred clear is pending.
    void RealizeDeferredClear();

    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffer();

    u64 SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore);

    void AllocateNewContext();

    void EndPendingOperations();

    void EndRenderPass();

    void AcquireNewChunk();

    const Device& device;
    StateTracker& state_tracker;

    std::unique_ptr<MasterSemaphore> master_semaphore;
    bool graphics_pipeline_missing{};
    bool fragment_barrier_pending{};
    bool render_pass_has_stores{};
    std::unique_ptr<CommandPool> command_pool;

    VideoCommon::QueryCacheBase<QueryCacheParams>* query_cache = nullptr;

    vk::CommandBuffer current_cmdbuf;
    vk::CommandBuffer current_upload_cmdbuf;

    DeferredClear deferred_clear;

    // Render pass hoisting (EnableRenderPassHoisting): the pending begin of the current render
    // pass, the main stream up to it, and the work recorded before it.
    struct PendingBegin {
        VkRenderPass renderpass{};
        VkFramebuffer framebuffer{};
        VkExtent2D render_area{};
        std::array<VkClearValue, 9> clear_values{};
        u32 clear_value_count{};
    };
    bool hoisting{};
    bool begin_pending{};
    PendingBegin pending_begin;
    std::unique_ptr<CommandChunk> outer_chunk;
    std::array<std::unique_ptr<CommandChunk>, PRE_PHASES> pre_chunks;

    std::unique_ptr<CommandChunk> chunk;
    std::function<void()> on_submit;
    std::function<void()> on_render_pass_end;

    /// EDEN_NX2_HANG_WATCH: the pipelines recorded into each of the last 64 submissions.
    struct HangSubmission {
        u64 tick{};
        std::vector<std::string> pipelines;
    };
    void HangWatchRecordGraphics(const std::array<u64, 6>& hashes);
    void HangWatchRecordCompute(u64 hash, u64 native_key);
    void HangRecord(std::string pipeline);
    void ReportStall(u64 tick, u64 gpu_tick, u64 next_tick, u64 waited_s);
    bool hang_watch{};
    std::vector<std::string> hang_work; ///< Worker thread: the submission being recorded
    std::mutex hang_mutex;
    std::deque<HangSubmission> hang_submissions;

    State state;
    u64 recording_epoch = 0;

    u32 num_renderpass_images = 0;
    std::array<VkImage, 9> renderpass_images{};
    std::array<VkImageSubresourceRange, 9> renderpass_image_ranges{};

    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    bool worker_waiting{}; ///< The worker is waiting for work (guarded by queue_mutex)
    std::jthread worker_thread;

    std::chrono::steady_clock::duration frame_interval{};
    std::chrono::steady_clock::time_point start_time{};
    double last_target_fps{};
    u64 max_frame_count{};
    u64 frame_counter{};
};

} // namespace Vulkan
