// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <fmt/format.h>
#include "common/scope_exit.h"
#include "common/scratch_buffer.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel::Svc {

/// Close a handle
Result CloseHandle(Core::System& system, Handle handle) {
    LOG_TRACE(Kernel_SVC, "Closing handle {:#08x}", handle);

    // Remove the handle.
    R_UNLESS(GetCurrentProcess(system.Kernel()).GetHandleTable().Remove(system.Kernel(), handle),
             ResultInvalidHandle);

    R_SUCCEED();
}

/// Clears the signaled state of an event or process.
Result ResetSignal(Core::System& system, Handle handle) {
    LOG_TRACE(Kernel_SVC, "called handle {:#08x}", handle);

    // Get the current handle table.
    const auto& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();

    // Try to reset as readable event.
    {
        KScopedAutoObject readable_event = handle_table.GetObject<KReadableEvent>(system.Kernel(), handle);
        if (readable_event.IsNotNull()) {
            R_RETURN(readable_event->Reset(system.Kernel()));
        }
    }

    // Try to reset as process.
    {
        KScopedAutoObject process = handle_table.GetObject<KProcess>(system.Kernel(), handle);
        if (process.IsNotNull()) {
            R_RETURN(process->Reset(system.Kernel()));
        }
    }

    R_THROW(ResultInvalidHandle);
}

/// Wait for the given handles to synchronize, timeout after the specified nanoseconds
namespace {

// With EDEN_NX2_SLEEP_STATS=1: every 10 s, the guest's waits with a timeout by its length: how
// many, how long they took, how many ran into the timeout and which threads those were.
bool WaitStats() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_SLEEP_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void NoteTimedWait(Core::System& system, s64 timeout_ns, s64 took_ns, bool timed_out) {
    struct Bucket {
        s64 limit_ns;
        u64 count{};
        u64 timed_out{};
        s64 took{};
        s64 took_timed_out{};
        std::map<std::string, u64> timed_out_threads;
    };
    static std::mutex mutex;
    static std::array<Bucket, 6> buckets{{{1'100'000}, {5'100'000}, {17'100'000}, {34'000'000},
                                          {110'000'000}, {(std::numeric_limits<s64>::max)()}}};
    static auto window_start = std::chrono::steady_clock::now();
    std::string name;
    if (timed_out) {
        name = Core::GetThreadName(&GetCurrentThread(system.Kernel())).value_or("?");
    }
    std::scoped_lock lock{mutex};
    for (Bucket& bucket : buckets) {
        if (timeout_ns <= bucket.limit_ns) {
            ++bucket.count;
            bucket.took += took_ns;
            if (timed_out) {
                ++bucket.timed_out;
                bucket.took_timed_out += took_ns;
                ++bucket.timed_out_threads[name];
            }
            break;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds{10}) {
        return;
    }
    std::string text;
    for (Bucket& bucket : buckets) {
        if (bucket.count != 0) {
            text += fmt::format(" | timeout up to {} us: {} waits, {} us on average, {} timed out",
                                bucket.limit_ns / 1000, bucket.count,
                                bucket.took / 1000 / static_cast<s64>(bucket.count),
                                bucket.timed_out);
            if (bucket.timed_out != 0) {
                text += fmt::format(" (after {} us:",
                                    bucket.took_timed_out / 1000 /
                                        static_cast<s64>(bucket.timed_out));
                for (const auto& [thread, count] : bucket.timed_out_threads) {
                    text += fmt::format(" {} {}", thread, count);
                }
                text += ")";
            }
        }
        bucket.count = 0;
        bucket.timed_out = 0;
        bucket.took = 0;
        bucket.took_timed_out = 0;
        bucket.timed_out_threads.clear();
    }
    LOG_INFO(Kernel_SVC, "Guest waits with a timeout in 10 s{}", text);
    window_start = now;
}

} // Anonymous namespace

Result WaitSynchronization(Core::System& system, int32_t* out_index, u64 user_handles,
                           int32_t num_handles, int64_t timeout_ns) {
    LOG_TRACE(Kernel_SVC, "called user_handles={:#x}, num_handles={}, timeout_ns={}", user_handles,
              num_handles, timeout_ns);

    // Ensure number of handles is valid.
    R_UNLESS(0 <= num_handles && num_handles <= Svc::ArgumentHandleCountMax, ResultOutOfRange);

    // Get the synchronization context.
    auto& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();
    auto objs = GetCurrentThread(system.Kernel()).GetSynchronizationObjectBuffer();
    auto handles = GetCurrentThread(system.Kernel()).GetHandleBuffer();

    // Copy user handles.
    if (num_handles > 0) {
        // Get the handles.
        R_UNLESS(GetCurrentMemory(system.Kernel()).ReadBlock(user_handles, handles.data(), sizeof(Handle) * num_handles), ResultInvalidPointer);

        // Convert the handles to objects.
        R_UNLESS(handle_table.GetMultipleObjects<KSynchronizationObject>(system.Kernel(), objs.data(), handles.data(), num_handles), ResultInvalidHandle);
    }

    // Ensure handles are closed when we're done.
    SCOPE_EXIT {
        for (auto i = 0; i < num_handles; ++i) {
            objs[i]->Close(system.Kernel());
        }
    };

    // Convert the timeout from nanoseconds to ticks.
    s64 timeout;
    if (timeout_ns > 0) {
        u64 ticks = system.Kernel().HardwareTimer().GetTick();
        ticks += timeout_ns;
        ticks += 2;

        timeout = ticks;
    } else {
        timeout = timeout_ns;
    }

    // Wait on the objects.
    const bool stats = timeout_ns > 0 && WaitStats();
    const auto wait_start =
        stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    Result res = KSynchronizationObject::Wait(system.Kernel(), out_index, objs.data(), num_handles, timeout);
    if (stats) {
        NoteTimedWait(system, timeout_ns,
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - wait_start)
                          .count(),
                      res == ResultTimedOut);
    }

    R_SUCCEED_IF(res == ResultSessionClosed);
    R_RETURN(res);
}

/// Resumes a thread waiting on WaitSynchronization
Result CancelSynchronization(Core::System& system, Handle handle) {
    LOG_TRACE(Kernel_SVC, "called handle={:#x}", handle);

    // Get the thread from its handle.
    KScopedAutoObject thread = GetCurrentProcess(system.Kernel()).GetHandleTable().GetObject<KThread>(system.Kernel(), handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    // Cancel the thread's wait.
    thread->WaitCancel(system.Kernel());
    R_SUCCEED();
}

void SynchronizePreemptionState(Core::System& system) {
    // Lock the scheduler.
    KScopedSchedulerLock sl{system.Kernel()};

    // If the current thread is pinned, unpin it.
    KProcess* cur_process = GetCurrentProcessPointer(system.Kernel());
    const auto core_id = GetCurrentCoreId(system.Kernel());

    if (cur_process->GetPinnedThread(core_id) == GetCurrentThreadPointer(system.Kernel())) {
        // Clear the current thread's interrupt flag.
        GetCurrentThread(system.Kernel()).ClearInterruptFlag(system.Kernel());

        // Unpin the current thread.
        cur_process->UnpinCurrentThread(system.Kernel());
    }
}

Result CloseHandle64(Core::System& system, Handle handle) {
    R_RETURN(CloseHandle(system, handle));
}

Result ResetSignal64(Core::System& system, Handle handle) {
    R_RETURN(ResetSignal(system, handle));
}

Result WaitSynchronization64(Core::System& system, int32_t* out_index, uint64_t handles,
                             int32_t num_handles, int64_t timeout_ns) {
    R_RETURN(WaitSynchronization(system, out_index, handles, num_handles, timeout_ns));
}

Result CancelSynchronization64(Core::System& system, Handle handle) {
    R_RETURN(CancelSynchronization(system, handle));
}

void SynchronizePreemptionState64(Core::System& system) {
    SynchronizePreemptionState(system);
}

Result CloseHandle64From32(Core::System& system, Handle handle) {
    R_RETURN(CloseHandle(system, handle));
}

Result ResetSignal64From32(Core::System& system, Handle handle) {
    R_RETURN(ResetSignal(system, handle));
}

Result WaitSynchronization64From32(Core::System& system, int32_t* out_index, uint32_t handles,
                                   int32_t num_handles, int64_t timeout_ns) {
    R_RETURN(WaitSynchronization(system, out_index, handles, num_handles, timeout_ns));
}

Result CancelSynchronization64From32(Core::System& system, Handle handle) {
    R_RETURN(CancelSynchronization(system, handle));
}

void SynchronizePreemptionState64From32(Core::System& system) {
    SynchronizePreemptionState(system);
}

} // namespace Kernel::Svc
