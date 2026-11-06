// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/fiber.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "common/settings.h"
#include "core/arm/nce_stats.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_interrupt_manager.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "video_core/gpu.h"

#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <sched.h>
#include <string>
#include "common/logging.h"
#endif

#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <sys/prctl.h>
#include "common/logging.h"
#ifndef PR_PAC_SET_ENABLED_KEYS
#define PR_PAC_SET_ENABLED_KEYS 60
#endif
#ifndef PR_PAC_APIAKEY
#define PR_PAC_APIAKEY (1UL << 0)
#define PR_PAC_APIBKEY (1UL << 1)
#define PR_PAC_APDAKEY (1UL << 2)
#define PR_PAC_APDBKEY (1UL << 3)
#endif
#endif

namespace Core {

#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
namespace {
// Android gives every thread its own PAC keys, and NCE runs guest PAC natively: a pointer the
// guest signs on one core thread (a fiber context, a return address) fails to authenticate on
// another. NCE core threads run with the instruction and data keys disabled, so PAC/AUT pass
// pointers through unchanged as under Dynarmic. PACGA keeps the process-wide GA key.
// EDEN_NX2_NCE_PAC_KEYS=1 keeps the keys.
constexpr unsigned long NcePacKeys =
    PR_PAC_APIAKEY | PR_PAC_APIBKEY | PR_PAC_APDAKEY | PR_PAC_APDBKEY;

bool DisableNcePacKeys() {
    if (!Settings::IsNceEnabled()) {
        return false;
    }
    static const bool keep_keys = [] {
        const char* const value = std::getenv("EDEN_NX2_NCE_PAC_KEYS");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    static std::once_flag logged;
    if (keep_keys) {
        std::call_once(logged, [] {
            LOG_INFO(Core, "NCE: PAC keys kept on the CPU core threads (EDEN_NX2_NCE_PAC_KEYS)");
        });
        return false;
    }
    const int result = prctl(PR_PAC_SET_ENABLED_KEYS, NcePacKeys, 0, 0, 0);
    const int error = errno;
    if (result != 0) {
        LOG_WARNING(Core, "NCE: could not disable the PAC keys on this CPU core thread (errno {})",
                    error);
        return false;
    }
    std::call_once(logged, [] {
        LOG_INFO(Core, "NCE: PAC instruction and data keys disabled on the CPU core threads");
    });
    return true;
}

// Before the thread returns, so the libc frames signed before the keys were disabled
// authenticate.
void RestoreNcePacKeys() {
    prctl(PR_PAC_SET_ENABLED_KEYS, NcePacKeys, NcePacKeys, 0, 0);
}
} // namespace
#endif

#ifdef __linux__
namespace {
// The emulated cores' host threads run at the GPU threads' priority and on the same CPUs, and a
// Switch 2 program keeps six of them busy. To A/B what that costs the GPU thread:
// EDEN_NCE_CORE_CPUS=<list> (e.g. 0-5) keeps them on these CPUs, leaving the others (the
// phone's prime cores) to the rest; EDEN_NCE_CORE_PRIORITY=normal|high|veryhigh puts them below
// the GPU threads (critical), which then win a contended CPU; EDEN_NCE_CORE_ADPF=0 leaves them
// out of the ADPF render session, whose boost packs them onto the prime cores with the GPU
// threads. Not re-applied when the app refreshes its thread policies (on resume).
void PlaceCoreThread(std::size_t core) {
    static const bool adpf = [] {
        const char* value = std::getenv("EDEN_NCE_CORE_ADPF");
        return value == nullptr || value[0] != '0';
    }();
    if (!adpf) {
        Common::SetCurrentThreadToAllCores();
        if (core == 0) {
            LOG_INFO(Core, "CPU core threads outside the ADPF session (EDEN_NCE_CORE_ADPF=0)");
        }
    }
    static const std::string cpus = [] {
        const char* value = std::getenv("EDEN_NCE_CORE_CPUS");
        return std::string{value != nullptr ? value : ""};
    }();
    static const std::string priority = [] {
        const char* value = std::getenv("EDEN_NCE_CORE_PRIORITY");
        return std::string{value != nullptr ? value : ""};
    }();
    if (!priority.empty()) {
        const auto level = priority == "normal"     ? Common::ThreadPriority::Normal
                           : priority == "high"     ? Common::ThreadPriority::High
                           : priority == "veryhigh" ? Common::ThreadPriority::VeryHigh
                                                    : Common::ThreadPriority::Critical;
        Common::SetCurrentThreadPriority(level);
    }
    if (cpus.empty()) {
        if (core == 0 && !priority.empty()) {
            LOG_INFO(Core, "CPU core threads at priority {} (EDEN_NCE_CORE_PRIORITY)", priority);
        }
        return;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    // "0-5" or "0,1,2" or both.
    for (size_t begin = 0; begin < cpus.size();) {
        size_t end = cpus.find(',', begin);
        if (end == std::string::npos) {
            end = cpus.size();
        }
        const std::string item = cpus.substr(begin, end - begin);
        const size_t dash = item.find('-');
        const int first = std::atoi(item.c_str());
        const int last = dash == std::string::npos ? first : std::atoi(item.c_str() + dash + 1);
        for (int cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu) {
            if (cpu >= 0) {
                CPU_SET(cpu, &mask);
            }
        }
        begin = end + 1;
    }
    const int result = sched_setaffinity(0, sizeof(mask), &mask);
    if (core == 0 || result != 0) {
        LOG_INFO(Core, "CPU core thread {} on CPUs {} (EDEN_NCE_CORE_CPUS){}{}", core, cpus,
                 priority.empty() ? "" : ", priority ", priority);
        if (result != 0) {
            LOG_WARNING(Core, "CPU core thread {}: sched_setaffinity failed (errno {})", core,
                        errno);
        }
    }
}
} // namespace
#endif

CpuManager::CpuManager(System& system_) : system{system_} {}
CpuManager::~CpuManager() = default;

void CpuManager::Initialize() {
    // After env.txt (System::Load): EDEN_NCE_STATS.
    Core::NceStats::Init();
    num_cores = is_multicore ? Core::Hardware::NUM_CPU_CORES : 1;
    gpu_barrier.emplace(num_cores + 1);
    for (std::size_t core = 0; core < num_cores; core++)
        core_data[core].host_thread = std::jthread([this, core](std::stop_token token) {
            RunThread(token, core);
        });
}

void CpuManager::Shutdown() {
    for (std::size_t core = 0; core < num_cores; core++) {
        if (core_data[core].host_thread.joinable()) {
            core_data[core].host_thread.request_stop();
            core_data[core].host_thread.join();
        }
    }
}

void CpuManager::GuestThreadFunction(Kernel::KernelCore& kernel) {
    if (is_multicore) {
        MultiCoreRunGuestThread(kernel);
    } else {
        SingleCoreRunGuestThread(kernel);
    }
}

void CpuManager::IdleThreadFunction(Kernel::KernelCore& kernel) {
    if (is_multicore) {
        MultiCoreRunIdleThread(kernel);
    } else {
        SingleCoreRunIdleThread(kernel);
    }
}

void CpuManager::ShutdownThreadFunction(Kernel::KernelCore& kernel) {
    ShutdownThread(kernel);
}

void CpuManager::HandleInterrupt(Kernel::KernelCore& kernel) {
    auto core_index = kernel.CurrentPhysicalCoreIndex();
    Kernel::KInterruptManager::HandleInterrupt(kernel, s32(core_index));
}

///////////////////////////////////////////////////////////////////////////////
///                             MultiCore                                   ///
///////////////////////////////////////////////////////////////////////////////

void CpuManager::MultiCoreRunGuestThread(Kernel::KernelCore& kernel) {
    // Similar to UserModeThreadStarter in HOS
    auto* thread = Kernel::GetCurrentThreadPointer(kernel);
    kernel.CurrentScheduler()->OnThreadStart(kernel);

    while (true) {
        auto* physical_core = &kernel.CurrentPhysicalCore();
        while (!physical_core->IsInterrupted()) {
            physical_core->RunThread(kernel, thread);
            physical_core = &kernel.CurrentPhysicalCore();
        }
        HandleInterrupt(kernel);
    }
}

void CpuManager::MultiCoreRunIdleThread(Kernel::KernelCore& kernel) {
    // Not accurate to HOS. Remove this entire method when singlecore is removed.
    // See notes in KScheduler::ScheduleImpl for more information about why this
    // is inaccurate.
    kernel.CurrentScheduler()->OnThreadStart(kernel);
    while (true) {
        auto& physical_core = kernel.CurrentPhysicalCore();
        if (!physical_core.IsInterrupted()) {
            physical_core.Idle();
        }
        HandleInterrupt(kernel);
    }
}

///////////////////////////////////////////////////////////////////////////////
///                             SingleCore                                   ///
///////////////////////////////////////////////////////////////////////////////

void CpuManager::SingleCoreRunGuestThread(Kernel::KernelCore& kernel) {
    auto* thread = Kernel::GetCurrentThreadPointer(kernel);
    kernel.CurrentScheduler()->OnThreadStart(kernel);

    while (true) {
        auto* physical_core = &kernel.CurrentPhysicalCore();
        if (!physical_core->IsInterrupted()) {
            physical_core->RunThread(kernel, thread);
            physical_core = &kernel.CurrentPhysicalCore();
        }

        kernel.SetIsPhantomModeForSingleCore(true);
        system.CoreTiming().Advance();
        kernel.SetIsPhantomModeForSingleCore(false);

        PreemptSingleCore(kernel);
        HandleInterrupt(kernel);
    }
}

void CpuManager::SingleCoreRunIdleThread(Kernel::KernelCore& kernel) {
    kernel.CurrentScheduler()->OnThreadStart(kernel);
    while (true) {
        PreemptSingleCore(kernel, false);
        system.CoreTiming().AddTicks(1000U);
        idle_count++;
        HandleInterrupt(kernel);
    }
}

void CpuManager::PreemptSingleCore(Kernel::KernelCore& kernel, bool from_running_environment) {
    if (idle_count >= 4 || from_running_environment) {
        if (!from_running_environment) {
            system.CoreTiming().Idle();
            idle_count = 0;
        }
        kernel.SetIsPhantomModeForSingleCore(true);
        system.CoreTiming().Advance();
        kernel.SetIsPhantomModeForSingleCore(false);
    }
    current_core.store((current_core + 1) % Core::Hardware::NUM_CPU_CORES);
    system.CoreTiming().ResetTicks();
    kernel.Scheduler(current_core).PreemptSingleCore(kernel);

    // We've now been scheduled again, and we may have exchanged schedulers.
    // Reload the scheduler in case it's different.
    if (!kernel.Scheduler(current_core).IsIdle()) {
        idle_count = 0;
    }
}

void CpuManager::GuestActivate(Kernel::KernelCore& kernel) {
    // Similar to the HorizonKernelMain callback in HOS
    auto* scheduler = kernel.CurrentScheduler();
    scheduler->Activate(kernel);
    UNREACHABLE();
}

void CpuManager::ShutdownThread(Kernel::KernelCore& kernel) {
    auto* thread = kernel.GetCurrentEmuThread();
    auto core = is_multicore ? kernel.CurrentPhysicalCoreIndex() : 0;
    Common::Fiber::YieldTo(thread->GetHostContext(), *core_data[core].host_context);
    UNREACHABLE();
}

void CpuManager::RunThread(std::stop_token token, std::size_t core) {
    /// Initialization
    system.RegisterCoreThread(core);
    std::string name = is_multicore ? ("CPUCore_" + std::to_string(core)) : std::string{"CPUThread"};
    Common::SetCurrentThreadName(name.c_str());
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
#ifdef __linux__
    PlaceCoreThread(core);
#endif
    auto& data = core_data[core];
    data.host_context = Common::Fiber::ThreadToFiber();

    // Cleanup
    SCOPE_EXIT {
        data.host_context->Exit();
    };

    // Running
    if (!gpu_barrier->Sync(token)) {
        return;
    }

    if (!is_async_gpu && !is_multicore) {
        system.GPU().ObtainContext();
    }

#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Before any guest code runs on this thread (see DisableNcePacKeys).
    const bool pac_keys_disabled = DisableNcePacKeys();
    SCOPE_EXIT {
        if (pac_keys_disabled) {
            RestoreNcePacKeys();
        }
    };
#endif

    auto& kernel = system.Kernel();
    auto& scheduler = *kernel.CurrentScheduler();
    auto* thread = scheduler.GetSchedulerCurrentThread();
    Kernel::SetCurrentThread(kernel, thread);

    Common::Fiber::YieldTo(data.host_context, *thread->GetHostContext());
}

} // namespace Core
