// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <map>
#include <string>
#include <vector>
#if defined(__aarch64__) && defined(__linux__)
#include <dlfcn.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "video_core/nx2_flight.h"
#include "video_core/nx2_sampler.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {
    hang_watch = master_semaphore->HangWatch();
    if (hang_watch) {
        master_semaphore->on_stall = [this](u64 tick, u64 gpu_tick, u64 next_tick,
                                            u64 waited_s) {
            ReportStall(tick, gpu_tick, next_tick, waited_s);
        };
    }

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

namespace {
void NotePassBreak(void* frame, int kind);
} // Anonymous namespace

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    NotePassBreak(__builtin_frame_address(0), 1);
    VideoCommon::Nx2Flight::Note(VideoCommon::Nx2Flight::Flushes);
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Flush");
    }
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    return signal_value;
}

bool Scheduler::DeferFlush(bool fence) {
    // Fences come one per compute release: ~11,500 a second in Hyrule Warriors: Age of
    // Imprisonment fights, each a queue submission (~22,000 a second with the others), which
    // kept the Vulkan worker busy submitting. Batched within 500 us: ~10-15% more fps there.
    // Command list copies after dispatches were ~9,000 more a second, of which the front waited
    // for about 5%.
    static const s64 interval_us = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_US");
        return value == nullptr ? s64{500} : static_cast<s64>(std::atoll(value));
    }();
    static const bool on_front = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_FRONT");
        return value == nullptr || value[0] != '0';
    }();
    const bool owner = VideoCommon::GpuSplit::OnBackThread() ||
                       (fence && on_front && VideoCommon::GpuSplit::OnGpuThread() &&
                        !VideoCommon::GpuSplit::Get().BackRunning());
    if (interval_us <= 0 || !defer_flush_allowed || !owner ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{interval_us}) {
        return false;
    }
    flush_pending = true;
    return true;
}

void Scheduler::FlushDeferred() {
    if (flush_pending) {
        Flush();
    }
}

bool Scheduler::WaitSubmitted(u64 tick) {
    if (IsSubmitted(tick)) {
        return true;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.RequestIdleHook();
    // No time limit: the back can spend seconds on one entry (a large shader's translation),
    // and a fence reported as submitted before it is would release its downloads too early.
    const auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds{2};
    for (u32 spins = 0; !IsSubmitted(tick); ++spins) {
        if (!split.BackRunning()) {
            return IsSubmitted(tick);
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_log) {
            next_log = now + std::chrono::seconds{2};
            LOG_WARNING(Render_Vulkan, "Waited {} s for the GPU thread to submit tick {}",
                        std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                        tick);
            split.RequestIdleHook();
        }
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }
    return true;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    NotePassBreak(__builtin_frame_address(0), 1);
    VideoCommon::Nx2Flight::Note(VideoCommon::Nx2Flight::Flushes);
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    CommitRenderPassBegin();
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        bool wake;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            wake = worker_waiting;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::Dispatches, 1);
        if (wake) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::WorkerWakes, 1);
            event_cv.notify_all();
        }
        AcquireNewChunk();
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    pending_begin = {
        .renderpass = renderpass,
        .framebuffer = framebuffer_handle,
        .render_area = render_area,
        .clear_value_count = (std::min)(clear_value_count, 9U),
    };
    for (u32 i = 0; i < pending_begin.clear_value_count; ++i) {
        pending_begin.clear_values[i] = clear_values[i];
    }
    if (hoisting) {
        // The pass's commands gather in a chunk of their own until it is handed over.
        ASSERT(!begin_pending);
        begin_pending = true;
        outer_chunk = std::move(chunk);
        chunk = TakeChunk();
    } else {
        RecordRenderPassBegin();
    }
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RecordRenderPassBegin() {
    Record([renderpass = pending_begin.renderpass, framebuffer_handle = pending_begin.framebuffer,
            render_area = pending_begin.render_area, values = pending_begin.clear_values,
            clear_value_count = pending_begin.clear_value_count](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
}

void Scheduler::CommitRenderPassBegin() {
    if (!begin_pending) {
        return;
    }
    begin_pending = false;
    VideoCommon::Nx2Sampler::g_render_passes.fetch_add(1, std::memory_order_relaxed);
    std::unique_ptr<CommandChunk> pass = std::move(chunk);
    chunk = std::move(outer_chunk);
    for (std::unique_ptr<CommandChunk>& phase : pre_chunks) {
        if (!phase) {
            continue;
        }
        if (phase->Empty()) {
            PushChunk(std::move(phase)); // to the reserve
            continue;
        }
        PushChunk(std::move(chunk));
        chunk = std::move(phase);
    }
    RecordRenderPassBegin();
    PushChunk(std::move(chunk));
    PushChunk(std::move(pass));
    chunk = TakeChunk();
}

std::array<bool, Scheduler::PRE_PHASES> Scheduler::PrepareBeforeRenderPass(
    const Framebuffer* framebuffer, size_t bytes) {
    RequestRenderpass(framebuffer);
    const bool room = std::ranges::all_of(pre_chunks, [bytes](const auto& phase) {
        return !phase || phase->HasRoom(bytes);
    });
    if (!begin_pending || !room) {
        // Its begin is handed over (or full): it ends, and begins again lazily after the work.
        EndRenderPass();
        RequestRenderpass(framebuffer);
    }
    std::array<bool, PRE_PHASES> empty{};
    for (size_t phase = 0; phase < PRE_PHASES; ++phase) {
        empty[phase] = !pre_chunks[phase] || pre_chunks[phase]->Empty();
    }
    return empty;
}

std::unique_ptr<Scheduler::CommandChunk> Scheduler::TakeChunk() {
    std::scoped_lock rl{reserve_mutex};
    if (chunk_reserve.empty()) {
        return std::make_unique<CommandChunk>();
    }
    std::unique_ptr<CommandChunk> taken = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
    return taken;
}

void Scheduler::PushChunk(std::unique_ptr<CommandChunk> work) {
    if (work->Empty()) {
        std::scoped_lock rl{reserve_mutex};
        chunk_reserve.emplace_back(std::move(work));
        return;
    }
    bool wake;
    {
        std::scoped_lock ql{queue_mutex};
        work_queue.push(std::move(work));
        wake = worker_waiting;
    }
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::Dispatches, 1);
    if (wake) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::WorkerWakes, 1);
        event_cv.notify_all();
    }
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer) {
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    // A draw continues the active pass whenever the same framebuffer and area are bound. The
    // render-pass *variant* (a deferred clear's loadOp=CLEAR form) is only meaningful at begin -
    // loadOps are applied once, and the base/variant passes are render-pass-compatible (only
    // loadOp/storeOp differ). Comparing the exact handle here made every draw after a deferred
    // clear restart the pass and re-LOAD what was just CLEARed (~118 MB of attachment traffic per
    // world-target turn). The framebuffer handle already fixes the attachment structure and key,
    // so it is the correct identity to compare.
    // EDEN_NX2_CLEAR_PASS_KEEP=0: the old exact compare, for A/B measurements.
    static const bool keep_clear_pass = [] {
        const char* const value = std::getenv("EDEN_NX2_CLEAR_PASS_KEEP");
        return value == nullptr || value[0] != '0';
    }();
    if ((keep_clear_pass ? state.renderpass != VkRenderPass{} : renderpass == state.renderpass) &&
        framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    Common::PinCurrentThreadFromEnvironment("EDEN_NX2_WORKER_CPUS");

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    // A reader waiting on a submission also waits for this thread to wake: with GpuWaitSpin,
    // look for work a moment before sleeping (from sylvieistaken/selevet-eglantine bce3c83739).
    const auto spin = GpuWaitSpin();

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        if (spin.count() > 0) {
            const auto end = std::chrono::steady_clock::now() + spin;
            do {
                {
                    std::scoped_lock lk{queue_mutex};
                    if (!work_queue.empty()) {
                        break;
                    }
                }
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < end && !stop_token.stop_requested());
        }

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] {
                worker_waiting = !TryPopQueue(work);
                return !worker_waiting;
            });
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
#ifdef __ANDROID__
        if (device.IsIntegrated()) {
            // Buffer downloads read buffer memory directly once their tick completes
            // (BufferCacheRuntime::CanReadBuffersDirectly): make the GPU's writes visible.
            static constexpr VkMemoryBarrier HOST_READ_BARRIER{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                   0, HOST_READ_BARRIER);
        }
#endif
        upload_cmdbuf.End();
        cmdbuf.End();

        if (hang_watch) {
            if (wait_semaphore != VK_NULL_HANDLE) {
                hang_work.insert(hang_work.begin(), "waits-on-semaphore");
            }
            std::scoped_lock hang_lock{hang_mutex};
            hang_submissions.push_back({signal_value, std::move(hang_work)});
            hang_work.clear();
            if (hang_submissions.size() > 64) {
                hang_submissions.pop_front();
            }
        }

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;
}

// EDEN_NX2_HANG_WATCH (pavelgajdo, Lichii-Android 492774519b): the pipelines each submission was
// recorded with, noted on the worker thread as the commands run, and a report of the submissions
// the GPU has not finished when a wait for one of them stalls. Tells a GPU-side hang (submitted,
// never finished) from an emulator deadlock (never submitted).
void Scheduler::HangWatchRecordGraphics(const std::array<u64, 6>& hashes) {
    Record([this, hashes](vk::CommandBuffer) {
        std::string name;
        for (size_t stage = 0; stage < hashes.size(); ++stage) {
            if (hashes[stage] != 0) {
                name += fmt::format("{}s{}:{:016x}", name.empty() ? "" : "+", stage,
                                    hashes[stage]);
            }
        }
        HangRecord(std::move(name));
    });
}

void Scheduler::HangWatchRecordCompute(u64 hash, u64 native_key) {
    Record([this, hash, native_key](vk::CommandBuffer) {
        HangRecord(native_key != 0 ? fmt::format("s6:{:016x}/native:{:016x}", hash, native_key)
                                   : fmt::format("s6:{:016x}", hash));
    });
}

void Scheduler::HangRecord(std::string pipeline) {
    if (hang_work.size() < 64 && std::ranges::find(hang_work, pipeline) == hang_work.end()) {
        hang_work.push_back(std::move(pipeline));
    }
}

void Scheduler::ReportStall(u64 tick, u64 gpu_tick, u64 next_tick, u64 waited_s) {
    std::scoped_lock lock{hang_mutex};
    const u64 last_submitted = hang_submissions.empty() ? 0 : hang_submissions.back().tick;
    LOG_WARNING(Render_Vulkan,
                "Hang watch: waited {} s for GPU tick {}; the GPU is at tick {}, the last "
                "submitted tick is {}, the next to record is {}",
                waited_s, tick, gpu_tick, last_submitted, next_tick);
    if (waited_s > 3) {
        return;
    }
    for (const HangSubmission& submission : hang_submissions) {
        if (submission.tick <= gpu_tick) {
            continue;
        }
        LOG_WARNING(Render_Vulkan, "Hang watch: unfinished tick {} ({} pipelines): {}",
                    submission.tick, submission.pipelines.size(),
                    fmt::join(submission.pipelines, " "));
    }
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    ++recording_epoch;
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    // EDEN_NX2_ZPASS_RESET=1 resets as before, for A/B.
    static const bool reset_counter = [] {
        const char* const value = std::getenv("EDEN_NX2_ZPASS_RESET");
        return value != nullptr && value[0] == '1';
    }();
    if (reset_counter) {
        query_cache->CounterReset(VideoCommon::QueryType::ZPassPixelCount64);
    } else {
        query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    }
    EndRenderPass();
}

namespace {
// EDEN_NX2_PASS_BREAKS=1 (ARM64 Linux and Android): every 10 s, the callers that ended the most
// render passes, as the return addresses above Scheduler::EndRenderPass (offsets in this library:
// llvm-addr2line names them). A title that draws ~1000 times a frame in ~100 passes of its own
// began ~820 host passes a frame on a phone, whose GPU pays for every one.
void NotePassBreak([[maybe_unused]] void* frame, [[maybe_unused]] int kind) {
#if defined(__aarch64__) && defined(__linux__)
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_PASS_BREAKS");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    using Key = std::array<u64, 5>;
    // The recording thread only. Kind 0: render passes ended, 1: submissions (Flush, Finish).
    static std::array<std::map<Key, u64>, 2> all_counts;
    static std::array<u64, 2> totals{};
    static std::array<std::chrono::steady_clock::time_point, 2> starts{
        std::chrono::steady_clock::now(), std::chrono::steady_clock::now()};
    std::map<Key, u64>& counts = all_counts[kind];
    u64& total = totals[kind];
    auto& window_start = starts[kind];
    Key key{};
    const u64* fp = static_cast<const u64*>(frame);
    for (size_t i = 0; i < key.size() && fp != nullptr; ++i) {
        const u64 saved = fp[1];
        u64 pc;
        // xpaclri: strips a pointer authentication code from x30.
        asm volatile("mov x30, %1; hint #7; mov %0, x30" : "=&r"(pc) : "r"(saved) : "x30");
        key[i] = pc;
        const u64* const next = reinterpret_cast<const u64*>(fp[0]);
        if (next <= fp || static_cast<u64>(next - fp) > (u64{1} << 17)) {
            break;
        }
        fp = next;
    }
    ++counts[key];
    ++total;
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds{10}) {
        return;
    }
    std::vector<std::pair<u64, Key>> top;
    top.reserve(counts.size());
    for (const auto& [pcs, count] : counts) {
        top.emplace_back(count, pcs);
    }
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.first > b.first; });
    Dl_info info{};
    const u64 base = dladdr(reinterpret_cast<void*>(&NotePassBreak), &info) != 0
                         ? reinterpret_cast<u64>(info.dli_fbase)
                         : 0;
    LOG_INFO(Render_Vulkan, "{} in 10 s: {} by {} callers",
             kind == 0 ? "Render passes ended" : "Submissions made", total, top.size());
    for (size_t i = 0; i < top.size() && i < 16; ++i) {
        std::string text;
        for (const u64 pc : top[i].second) {
            if (pc != 0) {
                text += fmt::format(" {:#x}", pc - base);
            }
        }
        LOG_INFO(Render_Vulkan, "{}: {} by{}", kind == 0 ? "Render pass enders" : "Submitters",
                 top[i].first, text);
    }
    counts.clear();
    total = 0;
    window_start = now;
#endif
}
} // Anonymous namespace

/// Finish the active render pass and invalidate recorded binds that do not survive it.
void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }
        NotePassBreak(__builtin_frame_address(0), 0);
        VideoCommon::Nx2Flight::Note(VideoCommon::Nx2Flight::PassEnds);
        ++recording_epoch;
        fragment_barrier_pending = false;
        render_pass_has_stores = false;
        render_pass_wrote_depth = false;

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);
        // A lazily begun pass: its begin and commands go before its end.
        CommitRenderPassBegin();

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
        if (on_render_pass_end) {
            on_render_pass_end();
        }
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
