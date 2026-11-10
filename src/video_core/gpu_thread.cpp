// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "common/assert.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/memory.h"
#include "core/hle/kernel/k_timer_task.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_thread.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_sampler.h"
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

// EDEN_NX2_PENDING_DUMP=<ms>: once a poll of suspended channels has lasted that long, log what the
// guest's threads wait for (the first 12 such polls, then every 100th): the semaphore the channel
// waits on is one the guest releases, so this says what the guest waits for in turn.
long PendingDumpMs() {
    static const long ms = [] {
        const char* const value = std::getenv("EDEN_NX2_PENDING_DUMP");
        return value != nullptr ? std::strtol(value, nullptr, 10) : 0L;
    }();
    return ms;
}

void DumpGuestWaits(Core::System& system, long waited_us) {
    static u64 dumps = 0;
    ++dumps;
    if (dumps > 12 && dumps % 100 != 0) {
        return;
    }
    Kernel::KProcess* const process = system.ApplicationProcess();
    if (process == nullptr) {
        return;
    }
    std::string text;
    // A thread's timer fires at its tick (a sleep's end, a timed wait's timeout): printed from
    // now, and only meaningful while the thread waits with one.
    const s64 now_tick = system.Kernel().HardwareTimer().GetTick();
    {
        Kernel::KScopedLightLock lk{process->GetListLock()};
        for (const Kernel::KThread& thread : process->GetThreadList()) {
            const auto name = Core::GetThreadName(&thread);
            std::string frames;
            const auto backtrace = Core::GetBacktrace(&thread);
            for (size_t i = 0; i < backtrace.size() && i < 5; ++i) {
                frames += fmt::format(" {}+{:#x}", backtrace[i].module, backtrace[i].offset);
            }
            text += fmt::format("\n  thread {} ({}): {} / {}, timer in {} us, at{}",
                                thread.GetThreadId(), name ? *name : std::string{"?"},
                                Core::GetThreadState(&thread), Core::GetThreadWaitReason(&thread),
                                (static_cast<const Kernel::KTimerTask&>(thread).GetTime() -
                                 now_tick) /
                                    1000,
                                frames);
            // Ocarina of Time's presentation thread (main+0x5704e0) sleeps 1 ms at a time until
            // a word reads 1: [[x22 + 0xe0 + 64 * [x22 + 0x164] + 8]]. Which word, and what the
            // other slots hold.
            if (name && *name == "Presentation Thread") {
                auto& memory = process->GetMemory();
                const u64 object = thread.GetContext().r[22];
                if (memory.IsValidVirtualAddressRange(object + 0xe0, 0x100)) {
                    const u32 index = memory.Read32(object + 0x164);
                    text += fmt::format(" | polls: x22 {:#x}, index {}", object, index);
                    for (u32 slot = 0; slot < 4; ++slot) {
                        const u64 entry = object + 0xe0 + 64 * slot;
                        const u64 pointer = memory.Read64(entry + 8);
                        Kernel::KMemoryInfo info{};
                        Kernel::Svc::PageInfo page_info{};
                        u32 state = ~0U;
                        if (process->GetPageTable()
                                .QueryInfo(std::addressof(info), std::addressof(page_info), pointer)
                                .IsSuccess()) {
                            state = static_cast<u32>(info.GetSvcState());
                        }
                        text += fmt::format(
                            ", slot {}: {:#x} {:#x} -> word at {:#x} (memory state {}) = {}", slot,
                            memory.Read64(entry), memory.Read64(entry + 16), pointer, state,
                            memory.IsValidVirtualAddressRange(pointer, 4)
                                ? fmt::format("{:#x}", memory.Read32(pointer))
                                : std::string{"unmapped"});
                    }
                }
            }
        }
    }
    LOG_INFO(HW_GPU, "Suspended channels polled for {} us (poll #{}), guest threads:{}", waited_us,
             dumps, text);
}

// The sleep between polls of suspended channels (Tegra::Nx2PendingSleepUs), asked each time:
// it is the running title's (read after env.txt and after the title is known, never at load).
std::chrono::microseconds PendingSleep() {
    const long us = Tegra::Nx2PendingSleepUs();
    // Once per title: which poll applies.
    static std::atomic<u64> logged{~u64{0}};
    if (const u64 program = Tegra::Nx2DefaultsProgramId();
        logged.load(std::memory_order_relaxed) != program &&
        logged.exchange(program, std::memory_order_relaxed) != program) {
        LOG_INFO(HW_GPU, "Suspended channels for program {:016X}: polled every {} us ({})",
                 program, us,
                 std::getenv("EDEN_NX2_PENDING_SLEEP_US") != nullptr ? "EDEN_NX2_PENDING_SLEEP_US"
                                                                     : "title default");
    }
    return std::chrono::microseconds{us};
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
#ifdef __linux__
        // The pending-channel polls below sleep as little as 50 us (Tegra::Nx2PendingSleepUs);
        // the default timer slack adds up to 50 more.
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
#endif
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::SetCurrentThreadToPerformanceCores();
        Common::PinCurrentThreadFromEnvironment("EDEN_NX2_FRONT_CPUS");
        namespace Sampler = VideoCommon::Nx2Sampler;
        const Sampler::Registration sampled{Sampler::Role::Front};
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the polling below).
        auto last_progress = std::chrono::steady_clock::now();
        bool pending_dumped = false;
        namespace Profile = VideoCommon::Nx2Profile;
        auto& split = VideoCommon::GpuSplit::Get();
        while (!stop_token.stop_requested()) {
            Profile::Report();
            split.MaybeStart(system);
            if (scheduler.HasPendingChannels()) {
                // Suspended channels wait on semaphores the CPU releases: poll them meanwhile.
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    scheduler.ResumePending(system.GPU());
                }
                if (!state.queue.TryPop(next)) {
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond, then in sleeps of the title's length (Tegra::Nx2PendingSleepUs:
                    // 50 us for Ocarina of Time, 1 ms elsewhere). A channel still waiting after
                    // 2 s (menus, loading) goes back to 1 ms sleeps.
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    Sampler::Scope mark{Sampler::FrontPending};
                    const auto waited = std::chrono::steady_clock::now() - last_progress;
                    if (const long dump_ms = PendingDumpMs();
                        dump_ms > 0 && !pending_dumped &&
                        waited >= std::chrono::milliseconds{dump_ms} &&
                        waited < std::chrono::seconds{1}) {
                        pending_dumped = true;
                        DumpGuestWaits(
                            system,
                            static_cast<long>(
                                std::chrono::duration_cast<std::chrono::microseconds>(waited)
                                    .count()));
                    }
                    if (waited < std::chrono::microseconds{500}) {
                        std::this_thread::yield();
                    } else if (waited < std::chrono::seconds{2}) {
                        std::this_thread::sleep_for(PendingSleep());
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
                Profile::Scope scope{Profile::Zone::Idle};
                Sampler::Scope mark{Sampler::FrontIdle};
                state.queue.PopWait(next, stop_token);
            }
            if (stop_token.stop_requested()) {
                break;
            }
            last_progress = std::chrono::steady_clock::now();
            pending_dumped = false;
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    Profile::Scope scope{Profile::Zone::Push};
                    // Held-back guest CPU writes first (see the single-thread path below).
                    if (Tegra::Nx2HeldWrites()) {
                        split.Defer([gpu = &system.GPU()] { gpu->InvalidateGPUCache(); });
                    }
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); });
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    if (run->before_later_reads) {
                        split.Drain(VideoCommon::Nx2Profile::Zone::DrainRun);
                        run->func();
                    } else {
                        split.DeferFunction(std::move(run->func));
                    }
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = flush->addr, size = flush->size] {
                        rasterizer_ptr->FlushRegion(addr, size);
                    });
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                        rasterizer_ptr->OnCacheInvalidation(addr, size);
                    });
                } else {
                    ASSERT(false);
                }
                split.Defer([state_ptr = &state, fence = next.fence, block = next.block] {
                    SignalCommand(*state_ptr, fence, block);
                });
                split.Publish();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Push};
                // Guest CPU writes to GPU-written memory are held back until the GPU thread
                // applies them, by default at the next fence. The guest made them before it
                // submitted this list, which may read them: apply them first. Pokemon Pokopia
                // keeps constants in pages its native storage bindings mark GPU-written, and some
                // frames drew stale ones (flickering shadows, stretched geometry). Switch 2
                // programs only; EDEN_NX2_HELD_WRITES=0 turns it off.
                if (Tegra::Nx2HeldWrites()) {
                    system.GPU().InvalidateGPUCache();
                }
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

    if (block) {
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

std::string ThreadManager::Nx2DebugState() const {
    return fmt::format("commands pushed {} signalled {}", state.last_fence,
                       state.signaled_fence.load(std::memory_order_relaxed));
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread
