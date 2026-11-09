// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <thread>
#if defined(ARCHITECTURE_x86_64)
#include <immintrin.h>
#endif

#include "common/assert.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

namespace {

// Tells a CPU thread waiting in PushCommand that its command ran.
void SignalCommand(SynchState& state, u64 fence, bool block) {
    state.signaled_fence.store(fence);
    if (block) {
        // We have to lock the write_lock to ensure that the condition_variable wait not get a
        // race between the check and the lock itself.
        std::scoped_lock lk{state.write_lock};
        state.cv.notify_all();
    }
}

// pending_wake: the GPU thread's pending sleeps, logged per second under EDEN_NX2_PERF_VERIFY or
// EDEN_NX2_GPU_PROFILE (GPU thread only).
struct PendingWakeStats {
    u64 waits{}, by_push{}, by_latch{}, timeouts{}, stale{}, skipped{};
    std::array<u32, 24> latency{}; ///< Latch to resume, bucket k: under 2^k us
    u32 latency_samples{};
    s64 window_start_ns{};
};

void NotePendingLatchResume(PendingWakeStats& stats, s64 latch_ns) {
    const s64 us = (Tegra::Nx2PendingWake::NowNs() - latch_ns) / 1000;
    const size_t bucket = std::min<size_t>(
        us <= 0 ? 0 : static_cast<size_t>(std::bit_width(static_cast<u64>(us))),
        stats.latency.size() - 1);
    ++stats.latency[bucket];
    ++stats.latency_samples;
}

void ReportPendingWake(PendingWakeStats& stats) {
    const s64 now = Tegra::Nx2PendingWake::NowNs();
    if (stats.window_start_ns == 0) {
        stats.window_start_ns = now;
        return;
    }
    if (now - stats.window_start_ns < 1'000'000'000) {
        return;
    }
    u64 p50_us = 0;
    for (u32 k = 0, seen = 0; k < stats.latency.size() && stats.latency_samples != 0; ++k) {
        seen += stats.latency[k];
        if (seen * 2 >= stats.latency_samples) {
            p50_us = u64{1} << k;
            break;
        }
    }
    LOG_INFO(HW_GPU,
             "Nx2Perf pending_wake: {} sleeps/s ended by push {}, latch {}, timeout {}, stale {} "
             "({} skipped before the wait); latch to resume p50 under {} us ({} samples); "
             "{} resumed unmet so far",
             stats.waits, stats.by_push, stats.by_latch, stats.timeouts, stats.stale,
             stats.skipped, p50_us, stats.latency_samples,
             Tegra::Nx2PendingWake::verify_unmet.load(std::memory_order_relaxed));
    stats = {};
    stats.window_start_ns = now;
}

/// pending_wake's form of the pending 1 ms sleep: ends early on a command queued or a latch
/// since the poll that saw `push_seen` and `latch_seen`.
void PendingWakeSleep(PendingWakeStats& stats, u64 push_seen, u64 latch_seen) {
    namespace Wake = Tegra::Nx2PendingWake;
    namespace Profile = VideoCommon::Nx2Profile;
    Wake::wake_event.Reset();
    Wake::wake_reasons.store(0, std::memory_order_relaxed);
    Wake::front_sleeping.store(true, std::memory_order_seq_cst);
    u32 reason = 0;
    if (Wake::push_seq.load(std::memory_order_seq_cst) != push_seen) {
        reason |= Wake::ByPush;
    }
    if (Wake::latch_seq.load(std::memory_order_seq_cst) != latch_seen) {
        reason |= Wake::ByLatch;
    }
    const bool skipped = reason != 0;
    bool woke = skipped;
    if (!skipped) {
        woke = Wake::wake_event.WaitFor(std::chrono::milliseconds{1});
        reason = woke ? Wake::wake_reasons.load(std::memory_order_relaxed) : 0;
    }
    Wake::front_sleeping.store(false, std::memory_order_seq_cst);
    // Verify checks the resumes the next polls make.
    Wake::verify_resume = reason != 0 && VideoCommon::Nx2Perf::Verify();

    const bool by_push = (reason & Wake::ByPush) != 0;
    const bool by_latch = !by_push && (reason & Wake::ByLatch) != 0;
    ++stats.waits;
    stats.by_push += by_push ? 1 : 0;
    stats.by_latch += by_latch ? 1 : 0;
    stats.timeouts += woke ? 0 : 1;
    stats.stale += woke && reason == 0 ? 1 : 0;
    stats.skipped += skipped ? 1 : 0;
    Profile::Add(Profile::Counter::PendingWaits, 1);
    Profile::Add(Profile::Counter::PendingWakePush, by_push ? 1 : 0);
    Profile::Add(Profile::Counter::PendingWakeLatch, by_latch ? 1 : 0);
    Profile::Add(Profile::Counter::PendingWakeTimeout, woke ? 0 : 1);
    if (VideoCommon::Nx2Perf::Verify() || Profile::Enabled()) {
        ReportPendingWake(stats);
    }
}

} // Anonymous namespace

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPU");
        VideoCommon::GpuSplit::MarkGpuThread();
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::SetCurrentThreadToPerformanceCores();
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the polling below).
        auto last_progress = std::chrono::steady_clock::now();
        namespace Profile = VideoCommon::Nx2Profile;
        auto& split = VideoCommon::GpuSplit::Get();
        const bool pending_wake = Tegra::Nx2PendingWake::On();
        PendingWakeStats pending_wake_stats;
        while (!stop_token.stop_requested()) {
            Profile::Report();
            split.MaybeStart(system);
            if (scheduler.HasPendingChannels()) {
                // pending_wake: the sleep below ends early on what comes after these.
                u64 push_seen{};
                u64 latch_seen{};
                s64 latch_ns_seen{};
                if (pending_wake) [[unlikely]] {
                    namespace Wake = Tegra::Nx2PendingWake;
                    push_seen = Wake::push_seq.load(std::memory_order_seq_cst);
                    latch_seen = Wake::latch_seq.load(std::memory_order_seq_cst);
                    latch_ns_seen = Wake::last_latch_ns.load(std::memory_order_relaxed);
                }
                // Suspended channels wait on semaphores the CPU releases: poll them meanwhile.
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    if (scheduler.ResumePending(system.GPU())) {
                        last_progress = std::chrono::steady_clock::now();
                        if (pending_wake) [[unlikely]] {
                            const s64 latch_ns = Tegra::Nx2PendingWake::last_latch_ns.exchange(
                                0, std::memory_order_relaxed);
                            if (latch_ns != 0) {
                                NotePendingLatchResume(pending_wake_stats, latch_ns);
                            }
                        }
                    } else if (pending_wake && latch_ns_seen != 0) [[unlikely]] {
                        // A latch this poll did not resume on: not one to time.
                        Tegra::Nx2PendingWake::last_latch_ns.compare_exchange_strong(
                            latch_ns_seen, 0, std::memory_order_relaxed);
                    }
                }
                if (pending_wake) [[unlikely]] {
                    Tegra::Nx2PendingWake::verify_resume = false;
                }
                if (!state.queue.TryPop(next)) {
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond before sleeping, or every wait also waits out a host sleep
                    // (title 1 waits ~25 times a second).
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    if (std::chrono::steady_clock::now() - last_progress <
                        std::chrono::microseconds{100}) {
#if defined(ARCHITECTURE_x86_64)
                        for (int i = 0; i < 32; ++i) {
                            _mm_pause();
                        }
#endif
                        std::this_thread::yield();
                    } else if (pending_wake) [[unlikely]] {
                        PendingWakeSleep(pending_wake_stats, push_seen, latch_seen);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
                Profile::Scope scope{Profile::Zone::Idle};
                state.queue.PopWait(next, stop_token);
            }
            if (stop_token.stop_requested()) {
                break;
            }
            if (!std::holds_alternative<GPUTickCommand>(next.data)) {
                last_progress = std::chrono::steady_clock::now();
            }
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    Profile::Scope scope{Profile::Zone::Push};
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); });
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    if (run->before_later_reads) {
                        // GPU memory maps unmap caches (UnmapMemory downloads): the back's state.
                        split.DeferFunction(std::move(run->func));
                        split.Drain(VideoCommon::DrainSite::Map);
                    } else {
                        split.DeferFunction(std::move(run->func));
                    }
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = flush->addr, size = flush->size] {
                        rasterizer_ptr->FlushRegion(addr, size);
                    });
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer(
                        [rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                            rasterizer_ptr->OnCacheInvalidation(addr, size);
                        },
                        VideoCommon::GpuSplit::Effect::None);
                    // It may drop programs that draws queued after it run: early MME reads
                    // past those draws check it like a noted write (MacroReadWriterSet).
                    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::MacroReadWriterSet)) {
                        split.NoteDeviceRange(invalidate->addr, invalidate->size);
                    }
                } else {
                    ASSERT(false);
                }
                split.Defer(
                    [state_ptr = &state, fence = next.fence, block = next.block] {
                        SignalCommand(*state_ptr, fence, block);
                    },
                    VideoCommon::GpuSplit::Effect::None);
                split.Publish();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Push};
                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                Profile::Scope scope{Profile::Zone::Tick};
                system.GPU().TickWork();
            } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Run};
                run->func();
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            SignalCommand(state, next.fence, next.block);
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::Run(std::function<void()>&& func, bool is_async, bool before_later_reads) {
    PushCommand(RunCommand{std::move(func), before_later_reads}, false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    state.queue.EmplaceWait(std::move(command_data), fence, block);
    if (Tegra::Nx2PendingWake::On()) [[unlikely]] {
        Tegra::Nx2PendingWake::Wake(Tegra::Nx2PendingWake::push_seq, Tegra::Nx2PendingWake::ByPush);
    }

    if (block) {
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread
