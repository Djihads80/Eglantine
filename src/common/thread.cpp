// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/adpf.h"
#include "common/error.h"
#include "common/event_wait.h"
#include "common/logging.h"
#include "common/assert.h"
#include "common/settings.h"
#include "common/thread.h"
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__HAIKU__)
#include <kernel/OS.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#include "common/windows/timer_resolution.h"
#else
#if defined(__FreeBSD__)
#include <sys/cpuset.h>
#include <sys/_cpuset.h>
#include <pthread_np.h>
// Compatibility with CPUset
#define cpu_set_t cpuset_t
#elif defined(__DragonFly__) || defined(__OpenBSD__) || defined(__Bitrig__)
#include <pthread_np.h>
#endif
#include <pthread.h>
#include <sched.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
#include <algorithm>
#include <fstream>

namespace {
constexpr int NICE_AUDIO = -16;
constexpr int NICE_URGENT_DISPLAY = -8;
constexpr int NICE_DISPLAY = -4;
constexpr int NICE_DEFAULT = 0;
constexpr int NICE_BACKGROUND = 10;

int LowestAllowedNice() {
    static const int lowest = [] {
        rlimit limit{};
        if (getrlimit(RLIMIT_NICE, &limit) != 0) {
            return 0;
        }
        if (limit.rlim_cur >= 40) {
            return -20;
        }
        return 20 - static_cast<int>(limit.rlim_cur);
    }();
    return lowest;
}

int NiceValueForPriority(Common::ThreadPriority priority) {
    const int wanted = [priority] {
        switch (priority) {
        case Common::ThreadPriority::Low: return NICE_BACKGROUND;
        case Common::ThreadPriority::Normal: return NICE_DEFAULT;
        case Common::ThreadPriority::High: return NICE_DISPLAY;
        case Common::ThreadPriority::VeryHigh: return NICE_URGENT_DISPLAY;
        case Common::ThreadPriority::Critical: return NICE_AUDIO;
        default: return NICE_DEFAULT;
        }
    }();
    return (std::max)(wanted, (std::min)(NICE_DEFAULT, LowestAllowedNice()));
}
} // Anonymous namespace
#endif

#ifdef __ANDROID__
#include <sys/utsname.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace {
constexpr size_t ANDROID_MINIMUM_PERFORMANCE_CORES = 4;

// A core counts as a performance core while it is within this much of the fastest one.
constexpr s64 ANDROID_PERFORMANCE_CAPACITY_PERCENT = 50;

constexpr std::chrono::nanoseconds ANDROID_POLICY_POLL_INTERVAL = std::chrono::milliseconds{500};

enum class CoreGroup {
    Unrestricted,
    Performance,
    Efficiency,
};

struct ThreadPolicy {
    pid_t tid;
    CoreGroup group;
    s32 nice_value;
    bool has_nice;
};

struct CoreInfo {
    s64 weight;
    u64 midr;
    s32 cpu;
};

struct CpuTopologyState {
    std::mutex topology_mutex;
    cpu_set_t allowed{};
    cpu_set_t performance{};
    cpu_set_t efficiency{};
    bool separated = false;
    bool initialized = false;

    pid_t canary_tid = 0;
    cpu_set_t canary_mask{};
    bool canary_valid = false;

    std::atomic<s64> next_poll_ns{0};

    std::mutex policy_mutex;
    std::vector<ThreadPolicy> policies;
};

CpuTopologyState& State() {
    static CpuTopologyState* const state = new CpuTopologyState();
    return *state;
}

struct PolicyRegistration {
    ~PolicyRegistration() {
        const pid_t tid = gettid();
        ::Common::ADPF::RemoveCurrentThread();
        CpuTopologyState& state = State();
        std::scoped_lock lock{state.policy_mutex};
        std::erase_if(state.policies,
                      [tid](const ThreadPolicy& policy) { return policy.tid == tid; });
    }
};

thread_local PolicyRegistration t_policy_registration;

s32 PossibleCpuCount() {
    std::ifstream file("/sys/devices/system/cpu/possible");
    std::string list;
    if (file && std::getline(file, list) && !list.empty()) {
        s64 highest = -1;
        const char* cursor = list.c_str();
        while (*cursor != '\0') {
            char* end = nullptr;
            const s64 value = std::strtol(cursor, &end, 10);
            if (end == cursor) {
                break;
            }
            highest = (std::max)(highest, value);
            cursor = end;
            while (*cursor == '-' || *cursor == ',') {
                ++cursor;
            }
        }
        if (highest >= 0) {
            return static_cast<s32>((std::min<s64>)(highest + 1, CPU_SETSIZE));
        }
    }
    const s64 configured = sysconf(_SC_NPROCESSORS_CONF);
    if (configured > 0) {
        return static_cast<s32>((std::min<s64>)(configured, CPU_SETSIZE));
    }
    return static_cast<s32>((std::min<s64>)(std::thread::hardware_concurrency(), CPU_SETSIZE));
}

s64 ReadCpuScalar(s32 cpu, const char* node) {
    s64 value = 0;
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/" + node);
    if (!file || !(file >> value) || value <= 0) {
        return 0;
    }
    return value;
}

u64 ReadCpuMidr(s32 cpu) {
    u64 midr = 0;
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                       "/regs/identification/midr_el1");
    if (!file || !(file >> std::hex >> midr)) {
        return 0;
    }
    return midr;
}

std::vector<CoreInfo> CollectCores(const cpu_set_t& allowed, s32 total, const char* node,
                                   bool require_all) {
    std::vector<CoreInfo> cores;
    for (s32 cpu = 0; cpu < total; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        const s64 weight = ReadCpuScalar(cpu, node);
        if (weight <= 0) {
            if (require_all) {
                return {};
            }
            LOG_WARNING(Common, "Could not read {} for CPU {}, treating it as an efficiency core",
                        node, cpu);
            continue;
        }
        cores.push_back(CoreInfo{weight, ReadCpuMidr(cpu), cpu});
    }
    return cores;
}

bool WeightsAreUniform(const std::vector<CoreInfo>& cores) {
    return std::all_of(cores.begin(), cores.end(),
                       [&](const CoreInfo& core) { return core.weight == cores.front().weight; });
}

bool MidrsAreDistinct(const std::vector<CoreInfo>& cores) {
    return std::none_of(cores.begin(), cores.end(),
                        [](const CoreInfo& core) { return core.midr == 0; }) &&
           std::any_of(cores.begin(), cores.end(),
                       [&](const CoreInfo& core) { return core.midr != cores.front().midr; });
}

void ComputeTopologyLocked(CpuTopologyState& state) {
    state.initialized = true;
    state.separated = false;
    CPU_ZERO(&state.allowed);
    CPU_ZERO(&state.performance);
    CPU_ZERO(&state.efficiency);

    if (sched_getaffinity(getpid(), sizeof(state.allowed), &state.allowed) != 0) {
        LOG_WARNING(Common, "Could not query process CPU affinity: {}",
                    ::Common::GetLastErrorMsg());
        return;
    }

    const s32 total = PossibleCpuCount();
    auto cores = CollectCores(state.allowed, total, "cpu_capacity", true);
    if (cores.empty() || (WeightsAreUniform(cores) && MidrsAreDistinct(cores))) {
        auto by_frequency = CollectCores(state.allowed, total, "cpufreq/cpuinfo_max_freq", false);
        if (!by_frequency.empty()) {
            cores = std::move(by_frequency);
        }
    }
    if (cores.empty()) {
        LOG_WARNING(Common, "Could not determine CPU topology, thread placement is disabled");
        return;
    }

    if (WeightsAreUniform(cores)) {
        if (MidrsAreDistinct(cores)) {
            LOG_WARNING(Common, "CPU clusters differ but rank identically, thread placement is "
                                "disabled");
        } else {
            LOG_INFO(Common, "CPU cores are symmetric, thread placement is disabled");
        }
        return;
    }

    std::sort(cores.begin(), cores.end(), [](const CoreInfo& lhs, const CoreInfo& rhs) {
        if (lhs.weight != rhs.weight) {
            return lhs.weight > rhs.weight;
        }
        return lhs.cpu < rhs.cpu;
    });

    const s64 fastest = cores.front().weight;
    size_t taken = 0;
    for (const auto& core : cores) {
        const bool fast_enough =
            core.weight * 100 >= fastest * ANDROID_PERFORMANCE_CAPACITY_PERCENT;
        if (!fast_enough && taken >= ANDROID_MINIMUM_PERFORMANCE_CORES) {
            break;
        }
        CPU_SET(core.cpu, &state.performance);
        ++taken;
    }
    if (taken == 0) {
        return;
    }

    for (s32 cpu = 0; cpu < total; ++cpu) {
        if (CPU_ISSET(cpu, &state.allowed) && !CPU_ISSET(cpu, &state.performance)) {
            CPU_SET(cpu, &state.efficiency);
        }
    }

    state.separated = CPU_COUNT(&state.efficiency) > 0;
    LOG_INFO(Common, "CPU topology: {} performance cores, {} efficiency cores, separation {}",
             CPU_COUNT(&state.performance), CPU_COUNT(&state.efficiency),
             state.separated ? "enabled" : "unavailable");
}

void EnsureTopologyLocked(CpuTopologyState& state) {
    if (!state.initialized) {
        ComputeTopologyLocked(state);
    }
}

void RefreshTopologyLocked(CpuTopologyState& state) {
    if (!state.initialized) {
        ComputeTopologyLocked(state);
        return;
    }
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(getpid(), sizeof(current), &current) != 0) {
        return;
    }
    if (std::memcmp(&current, &state.allowed, sizeof(current)) != 0) {
        ComputeTopologyLocked(state);
    }
}

bool ApplyCoreGroupLocked(CpuTopologyState& state, pid_t tid, CoreGroup group,
                          bool* gone = nullptr) {
    const bool restrict_group = group != CoreGroup::Unrestricted && state.separated;
    const cpu_set_t* mask = &state.allowed;
    if (restrict_group) {
        mask = group == CoreGroup::Performance ? &state.performance : &state.efficiency;
    }
    if (CPU_COUNT(mask) == 0) {
        return false;
    }
    if (sched_setaffinity(tid, sizeof(*mask), mask) != 0) {
        if (gone != nullptr && errno == ESRCH) {
            *gone = true;
            return false;
        }
        LOG_WARNING(Common, "Could not restrict thread {} to its core group: {}", tid,
                    ::Common::GetLastErrorMsg());
        return false;
    }
    return true;
}

bool KernelPreservesRequestedAffinity() {
    utsname info{};
    if (uname(&info) != 0) {
        return false;
    }
    s32 major = 0;
    s32 minor = 0;
    if (std::sscanf(info.release, "%d.%d", &major, &minor) != 2) {
        return false;
    }
    return major > 6 || (major == 6 && minor >= 2);
}

void SnapshotCanaryLocked(CpuTopologyState& state) {
    state.canary_valid = false;
    if (!state.separated) {
        return;
    }
    for (const auto& policy : state.policies) {
        if (policy.group == CoreGroup::Unrestricted) {
            continue;
        }
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(policy.tid, sizeof(mask), &mask) != 0) {
            continue;
        }
        state.canary_tid = policy.tid;
        state.canary_mask = mask;
        state.canary_valid = true;
        return;
    }
}

bool DueForPoll(CpuTopologyState& state) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    s64 next = state.next_poll_ns.load(std::memory_order_relaxed);
    if (now_ns < next) {
        return false;
    }
    return state.next_poll_ns.compare_exchange_strong(
        next, now_ns + ANDROID_POLICY_POLL_INTERVAL.count(), std::memory_order_relaxed);
}

ThreadPolicy& AcquirePolicyLocked(CpuTopologyState& state, pid_t tid) {
    for (auto& policy : state.policies) {
        if (policy.tid == tid) {
            return policy;
        }
    }
    return state.policies.emplace_back(ThreadPolicy{tid, CoreGroup::Unrestricted, 0, false});
}

void SetCurrentThreadCoreGroup(CoreGroup group) {
    const pid_t tid = gettid();
    (void)&t_policy_registration;

    CoreGroup effective = group;
    if (tid == getpid() && group != CoreGroup::Unrestricted) {
        LOG_WARNING(Common, "Refusing to place the main thread: the CPU topology is read from it");
        effective = CoreGroup::Unrestricted;
    }

    CpuTopologyState& state = State();
    std::scoped_lock topology_lock{state.topology_mutex};
    EnsureTopologyLocked(state);
    ApplyCoreGroupLocked(state, tid, effective);

    std::scoped_lock policy_lock{state.policy_mutex};
    AcquirePolicyLocked(state, tid).group = effective;
    if (!state.canary_valid) {
        SnapshotCanaryLocked(state);
    }
}

void RememberCurrentThreadNice(pid_t tid, s32 nice_value) {
    (void)&t_policy_registration;
    CpuTopologyState& state = State();
    std::scoped_lock lock{state.policy_mutex};
    ThreadPolicy& policy = AcquirePolicyLocked(state, tid);
    policy.nice_value = nice_value;
    policy.has_nice = true;
}
} // Anonymous namespace
#endif

#include "common/cpu_features.h"
#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include "common/x64/rdtsc.h"
#endif

namespace Common {

#ifdef _WIN32
namespace {

struct Win32CacheTopology {
    DWORD_PTR process_mask = 0;
    DWORD_PTR performance_mask = 0;
    DWORD_PTR efficiency_mask = 0;
    bool asymmetric = false;
};

const Win32CacheTopology& GetWin32CacheTopology() {
    static const Win32CacheTopology topology = [] {
        Win32CacheTopology result{};
        DWORD_PTR system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &result.process_mask, &system_mask) ||
            result.process_mask == 0) {
            return result;
        }
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
        if (length == 0) {
            return result;
        }
        std::vector<u8> buffer(length);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (!GetLogicalProcessorInformationEx(RelationCache, info, &length)) {
            return result;
        }
        DWORD max_l3_size = 0;
        DWORD min_l3_size = (std::numeric_limits<DWORD>::max)();
        struct L3Domain {
            DWORD size;
            DWORD_PTR mask;
        };
        std::vector<L3Domain> domains;
        for (DWORD offset = 0; offset < length;) {
            const auto* entry =
                reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (entry->Size == 0) {
                break;
            }
            if (entry->Relationship == RelationCache && entry->Cache.Level == 3 &&
                (entry->Cache.Type == CacheUnified || entry->Cache.Type == CacheData)) {
                DWORD_PTR domain_mask = 0;
                const WORD group_count = entry->Cache.GroupCount > 0 ? entry->Cache.GroupCount : 1;
                for (WORD g = 0; g < group_count; ++g) {
                    if (entry->Cache.GroupMasks[g].Group == 0) {
                        domain_mask |= entry->Cache.GroupMasks[g].Mask & result.process_mask;
                    }
                }
                if (domain_mask != 0 && entry->Cache.CacheSize > 0) {
                    max_l3_size = (std::max)(max_l3_size, entry->Cache.CacheSize);
                    min_l3_size = (std::min)(min_l3_size, entry->Cache.CacheSize);
                    domains.push_back({entry->Cache.CacheSize, domain_mask});
                }
            }
            offset += entry->Size;
        }
        if (domains.size() >= 2 && max_l3_size > min_l3_size) {
            for (const auto& domain : domains) {
                if (domain.size == max_l3_size) {
                    result.performance_mask |= domain.mask;
                } else {
                    result.efficiency_mask |= domain.mask;
                }
            }
            result.efficiency_mask &= ~result.performance_mask;
            result.asymmetric = result.performance_mask != 0 && result.efficiency_mask != 0;
            if (result.asymmetric) {
                LOG_INFO(Common,
                         "Detected asymmetric L3 cache ({} MiB vs {} MiB): 3D V-Cache mask={:#x}, "
                         "secondary CCD mask={:#x}",
                         max_l3_size >> 20, min_l3_size >> 20, result.performance_mask,
                         result.efficiency_mask);
            }
        }
        return result;
    }();
    return topology;
}

bool IsVCachePinningEnabled() {
    static const int env_override = [] {
        if (const char* env = std::getenv("EDEN_NX2_VCACHE")) {
            return env[0] == '0' ? 0 : 1;
        }
        return -1;
    }();
    if (env_override >= 0) {
        return env_override != 0;
    }
    return Settings::values.prefer_3d_vcache.GetValue();
}

enum class Win32CoreGroup {
    Unrestricted,
    Performance,
    Efficiency,
};

struct Win32ThreadPolicy {
    DWORD tid;
    HANDLE handle;
    Win32CoreGroup group;
    DWORD_PTR pinned_mask = 0; ///< PinCurrentThreadToGpuSlot: replaces the group's mask
};

struct Win32PolicyState {
    std::mutex mutex;
    std::vector<Win32ThreadPolicy> policies;
};

Win32PolicyState& GetWin32PolicyState() {
    static Win32PolicyState* const state = new Win32PolicyState();
    return *state;
}

/// ReserveGpuCores: the CPUs kept for the GPU threads (slots, see GpuPinMode).
struct Win32GpuReservation {
    std::once_flag once;
    std::atomic<DWORD_PTR> reserved_mask{0}; ///< Group 0; published after the fields below
    GpuPinMode mode = GpuPinMode::Pair;
    std::array<int, 3> slot_cpu{-1, -1, -1}; ///< Logical CPU in group 0 per slot
    std::array<ULONG, 3> slot_cpu_set{};     ///< Its CPU set id (0 = none)
    std::atomic<u32> placement_logs{0};
};

Win32GpuReservation& GetWin32GpuReservation() {
    static Win32GpuReservation* const reservation = new Win32GpuReservation();
    return *reservation;
}

DWORD_PTR ReservedGpuMask() {
    return GetWin32GpuReservation().reserved_mask.load(std::memory_order_acquire);
}

DWORD_PTR MaskForWin32Group(const Win32CacheTopology& topology, Win32CoreGroup group,
                            bool vcache_enabled) {
    const DWORD_PTR mask = [&] {
        if (!vcache_enabled || group == Win32CoreGroup::Unrestricted) {
            return topology.process_mask;
        }
        return group == Win32CoreGroup::Performance ? topology.performance_mask
                                                    : topology.efficiency_mask;
    }();
    const DWORD_PTR reserved = ReservedGpuMask();
    if (reserved == 0) {
        return mask;
    }
    if ((mask & ~reserved) != 0) {
        return mask & ~reserved;
    }
    // Never an empty mask: the group lies inside the reservation, so take every other CPU.
    return (topology.process_mask & ~reserved) != 0 ? topology.process_mask & ~reserved : mask;
}

DWORD_PTR MaskForWin32Policy(const Win32CacheTopology& topology, const Win32ThreadPolicy& policy,
                             bool vcache_enabled) {
    return policy.pinned_mask != 0 ? policy.pinned_mask
                                   : MaskForWin32Group(topology, policy.group, vcache_enabled);
}

std::string Win32ThreadName(HANDLE thread) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandle(TEXT("KernelBase.dll")),
                                               "GetThreadDescription")));
    if (get_description == nullptr) {
        return {};
    }
    PWSTR description = nullptr;
    if (FAILED(get_description(thread, &description)) || description == nullptr) {
        return {};
    }
    std::string name = UTF16ToUTF8(std::wstring_view{description});
    LocalFree(description);
    return name;
}

const char* Win32GroupName(Win32CoreGroup group) {
    switch (group) {
    case Win32CoreGroup::Performance:
        return "performance";
    case Win32CoreGroup::Efficiency:
        return "efficiency";
    default:
        return "unrestricted";
    }
}

const char* GpuPinModeName(GpuPinMode mode) {
    switch (mode) {
    case GpuPinMode::Separate:
        return "separate";
    case GpuPinMode::PairWorker:
        return "pair+worker";
    case GpuPinMode::SeparateWorker:
        return "separate+worker";
    default:
        return "pair";
    }
}

struct Win32PolicyRegistration {
    ~Win32PolicyRegistration() {
        const DWORD tid = GetCurrentThreadId();
        Win32PolicyState& state = GetWin32PolicyState();
        std::scoped_lock lock{state.mutex};
        for (auto it = state.policies.begin(); it != state.policies.end(); ++it) {
            if (it->tid == tid) {
                if (it->handle != nullptr) {
                    CloseHandle(it->handle);
                }
                state.policies.erase(it);
                break;
            }
        }
    }
};

thread_local Win32PolicyRegistration t_win32_policy_registration;

void SetCurrentThreadWin32Group(Win32CoreGroup group) {
    const auto& topology = GetWin32CacheTopology();
    if (!topology.asymmetric) {
        return;
    }
    (void)&t_win32_policy_registration;
    const DWORD tid = GetCurrentThreadId();
    const DWORD_PTR mask = MaskForWin32Group(topology, group, IsVCachePinningEnabled());
    SetThreadAffinityMask(GetCurrentThread(), mask);
    if (const DWORD_PTR reserved = ReservedGpuMask(); reserved != 0) {
        auto& reservation = GetWin32GpuReservation();
        if (reservation.placement_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_INFO(Common, "gpu_core_pin: thread {} ({}) {} mask={:#x}",
                     Win32ThreadName(GetCurrentThread()), tid, Win32GroupName(group), mask);
        }
        if ((mask & reserved) != 0) {
            LOG_ERROR(Common, "Nx2Perf gpu_core_pin: thread {} mask {:#x} includes reserved {:#x}",
                      tid, mask, reserved);
        }
    }

    Win32PolicyState& state = GetWin32PolicyState();
    std::scoped_lock lock{state.mutex};
    for (auto& policy : state.policies) {
        if (policy.tid == tid) {
            policy.group = group;
            policy.pinned_mask = 0;
            return;
        }
    }
    HANDLE dup_handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup_handle,
                        THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        state.policies.push_back(Win32ThreadPolicy{tid, dup_handle, group});
    }
}

/// Group 0 physical cores (SMT sibling masks) inside the process affinity.
std::vector<DWORD_PTR> ReadWin32PhysicalCores(DWORD_PTR process_mask) {
    std::vector<DWORD_PTR> cores;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return cores;
    }
    std::vector<u8> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        return cores;
    }
    for (DWORD offset = 0; offset < length;) {
        const auto* entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        if (entry->Relationship == RelationProcessorCore) {
            DWORD_PTR mask = 0;
            for (WORD g = 0; g < entry->Processor.GroupCount; ++g) {
                if (entry->Processor.GroupMask[g].Group == 0) {
                    mask |= entry->Processor.GroupMask[g].Mask & process_mask;
                }
            }
            if (mask != 0) {
                cores.push_back(mask);
            }
        }
        offset += entry->Size;
    }
    return cores;
}

/// Moves the process default CPU sets off the reserved CPUs (threads nobody pins: driver, shader
/// workers, audio) and notes each slot CPU's set id.
void ApplyGpuReservationCpuSets(Win32GpuReservation& reservation, DWORD_PTR reserved) {
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        LOG_WARNING(Common, "gpu_core_pin: GetSystemCpuSetInformation failed ({})", GetLastError());
        return;
    }
    std::vector<u8> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                    length, &length, GetCurrentProcess(), 0)) {
        LOG_WARNING(Common, "gpu_core_pin: GetSystemCpuSetInformation failed ({})", GetLastError());
        return;
    }
    std::vector<ULONG> allowed;
    for (ULONG offset = 0; offset < length;) {
        const auto* entry =
            reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        if (entry->Type == CpuSetInformation) {
            const BYTE cpu = entry->CpuSet.LogicalProcessorIndex;
            const bool is_reserved = entry->CpuSet.Group == 0 && cpu < sizeof(DWORD_PTR) * 8 &&
                                     ((reserved >> cpu) & 1) != 0;
            if (is_reserved) {
                for (size_t slot = 0; slot < reservation.slot_cpu.size(); ++slot) {
                    if (reservation.slot_cpu[slot] == static_cast<int>(cpu)) {
                        reservation.slot_cpu_set[slot] = entry->CpuSet.Id;
                    }
                }
            } else {
                allowed.push_back(entry->CpuSet.Id);
            }
        }
        offset += entry->Size;
    }
    if (allowed.empty()) {
        LOG_WARNING(Common, "gpu_core_pin: no CPU set outside the reservation");
        return;
    }
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), allowed.data(),
                                  static_cast<ULONG>(allowed.size()))) {
        LOG_WARNING(Common, "gpu_core_pin: SetProcessDefaultCpuSets failed ({})", GetLastError());
        return;
    }
    LOG_INFO(Common, "gpu_core_pin: process default CPU sets: {} CPUs outside the reservation",
             allowed.size());
}

void LogWin32Placement(DWORD_PTR reserved) {
    const auto& topology = GetWin32CacheTopology();
    const bool vcache_enabled = IsVCachePinningEnabled();
    Win32PolicyState& state = GetWin32PolicyState();
    std::scoped_lock lock{state.mutex};
    std::string line;
    for (const auto& policy : state.policies) {
        const DWORD_PTR mask = MaskForWin32Policy(topology, policy, vcache_enabled);
        line += fmt::format(" | {} ({}) {} mask={:#x}", Win32ThreadName(policy.handle), policy.tid,
                            Win32GroupName(policy.group), mask);
        if (policy.pinned_mask == 0 && (mask & reserved) != 0) {
            LOG_ERROR(Common, "Nx2Perf gpu_core_pin: thread {} mask {:#x} includes reserved {:#x}",
                      policy.tid, mask, reserved);
        }
    }
    LOG_INFO(Common, "gpu_core_pin: registered threads after the refresh:{}", line);
}

void ReserveWin32GpuCores(Win32GpuReservation& reservation, GpuPinMode mode) {
    const auto& topology = GetWin32CacheTopology();
    if (!topology.asymmetric) {
        LOG_INFO(Common, "gpu_core_pin: no asymmetric L3 cache, nothing reserved");
        return;
    }
    // V-Cache physical cores other than core 0, highest-numbered first.
    std::vector<DWORD_PTR> candidates;
    for (const DWORD_PTR core : ReadWin32PhysicalCores(topology.process_mask)) {
        const DWORD_PTR mask = core & topology.performance_mask;
        if (mask != 0 && (core & 1) == 0) {
            candidates.push_back(mask);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](DWORD_PTR a, DWORD_PTR b) {
        return std::countl_zero(a) < std::countl_zero(b);
    });
    const auto low_cpu = [](DWORD_PTR mask) { return std::countr_zero(mask); };
    const DWORD_PTR front_core = candidates.empty() ? 0 : candidates[0];
    // Pair mode on a core without an SMT sibling places the back on the next core instead.
    const bool pair = mode == GpuPinMode::Pair || mode == GpuPinMode::PairWorker;
    const bool worker = mode == GpuPinMode::PairWorker || mode == GpuPinMode::SeparateWorker;
    const bool back_on_sibling = pair && std::popcount(front_core) >= 2;
    const size_t needed = 1 + (back_on_sibling ? 0 : 1) + (worker ? 1 : 0);
    if (candidates.size() < needed) {
        LOG_WARNING(Common,
                    "gpu_core_pin: {} V-Cache cores besides core 0, {} needed; nothing reserved",
                    candidates.size(), needed);
        return;
    }
    size_t next = 1;
    DWORD_PTR reserved = front_core;
    reservation.mode = mode;
    reservation.slot_cpu[0] = low_cpu(front_core);
    if (back_on_sibling) {
        reservation.slot_cpu[1] = low_cpu(front_core & (front_core - 1));
    } else {
        reserved |= candidates[next];
        reservation.slot_cpu[1] = low_cpu(candidates[next++]);
    }
    if (worker) {
        reserved |= candidates[next];
        reservation.slot_cpu[2] = low_cpu(candidates[next++]);
    }
    ApplyGpuReservationCpuSets(reservation, reserved);
    reservation.reserved_mask.store(reserved, std::memory_order_release);
    LOG_INFO(Common,
             "gpu_core_pin: mode {} reserved_mask={:#x} (front CPU {}, back CPU {}, worker CPU {})",
             GpuPinModeName(mode), reserved, reservation.slot_cpu[0], reservation.slot_cpu[1],
             reservation.slot_cpu[2]);
    RefreshThreadPolicies();
    LogWin32Placement(reserved);
}

} // Anonymous namespace
#elif defined(__linux__) && !defined(__ANDROID__)
namespace {

enum class LinuxCoreGroup {
    Unrestricted,
    Performance,
    Efficiency,
};

struct LinuxCacheTopology {
    cpu_set_t process_mask{};
    cpu_set_t performance_mask{};
    cpu_set_t efficiency_mask{};
    bool asymmetric = false;
};

u64 ReadLinuxL3CacheSize(s32 cpu) {
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index3/size");
    std::string text;
    if (!file || !(file >> text) || text.empty()) {
        return 0;
    }
    char* end = nullptr;
    u64 value = std::strtoull(text.c_str(), &end, 10);
    if (end != nullptr) {
        if (*end == 'K' || *end == 'k') {
            value <<= 10;
        } else if (*end == 'M' || *end == 'm') {
            value <<= 20;
        }
    }
    return value;
}

const LinuxCacheTopology& GetLinuxCacheTopology() {
    static const LinuxCacheTopology topology = [] {
        LinuxCacheTopology result{};
        CPU_ZERO(&result.process_mask);
        CPU_ZERO(&result.performance_mask);
        CPU_ZERO(&result.efficiency_mask);
        if (sched_getaffinity(0, sizeof(result.process_mask), &result.process_mask) != 0) {
            return result;
        }
        u64 max_l3 = 0;
        u64 min_l3 = (std::numeric_limits<u64>::max)();
        std::vector<std::pair<s32, u64>> cpu_l3;
        for (s32 cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &result.process_mask)) {
                continue;
            }
            const u64 l3 = ReadLinuxL3CacheSize(cpu);
            if (l3 > 0) {
                max_l3 = (std::max)(max_l3, l3);
                min_l3 = (std::min)(min_l3, l3);
                cpu_l3.emplace_back(cpu, l3);
            }
        }
        if (cpu_l3.size() >= 2 && max_l3 > min_l3) {
            for (const auto& [cpu, l3] : cpu_l3) {
                if (l3 == max_l3) {
                    CPU_SET(cpu, &result.performance_mask);
                } else {
                    CPU_SET(cpu, &result.efficiency_mask);
                }
            }
            result.asymmetric =
                CPU_COUNT(&result.performance_mask) > 0 && CPU_COUNT(&result.efficiency_mask) > 0;
            if (result.asymmetric) {
                LOG_INFO(Common,
                         "Detected asymmetric L3 cache ({} MiB vs {} MiB): {} 3D V-Cache threads, "
                         "{} secondary threads",
                         max_l3 >> 20, min_l3 >> 20, CPU_COUNT(&result.performance_mask),
                         CPU_COUNT(&result.efficiency_mask));
            }
        }
        return result;
    }();
    return topology;
}

bool IsVCachePinningEnabled() {
    static const int env_override = [] {
        if (const char* env = std::getenv("EDEN_NX2_VCACHE")) {
            return env[0] == '0' ? 0 : 1;
        }
        return -1;
    }();
    if (env_override >= 0) {
        return env_override != 0;
    }
    return Settings::values.prefer_3d_vcache.GetValue();
}

struct LinuxThreadPolicy {
    pid_t tid;
    LinuxCoreGroup group;
};

struct LinuxPolicyState {
    std::mutex mutex;
    std::vector<LinuxThreadPolicy> policies;
};

LinuxPolicyState& GetLinuxPolicyState() {
    static LinuxPolicyState* const state = new LinuxPolicyState();
    return *state;
}

const cpu_set_t& MaskForLinuxGroup(const LinuxCacheTopology& topology, LinuxCoreGroup group,
                                   bool vcache_enabled) {
    if (!vcache_enabled || group == LinuxCoreGroup::Unrestricted) {
        return topology.process_mask;
    }
    return group == LinuxCoreGroup::Performance ? topology.performance_mask
                                                : topology.efficiency_mask;
}

struct LinuxPolicyRegistration {
    ~LinuxPolicyRegistration() {
        const pid_t tid = gettid();
        LinuxPolicyState& state = GetLinuxPolicyState();
        std::scoped_lock lock{state.mutex};
        std::erase_if(state.policies,
                      [tid](const LinuxThreadPolicy& policy) { return policy.tid == tid; });
    }
};

thread_local LinuxPolicyRegistration t_linux_policy_registration;

void SetCurrentThreadLinuxGroup(LinuxCoreGroup group) {
    const auto& topology = GetLinuxCacheTopology();
    if (!topology.asymmetric) {
        return;
    }
    (void)&t_linux_policy_registration;
    const pid_t tid = gettid();
    const auto& mask = MaskForLinuxGroup(topology, group, IsVCachePinningEnabled());
    sched_setaffinity(tid, sizeof(mask), &mask);

    LinuxPolicyState& state = GetLinuxPolicyState();
    std::scoped_lock lock{state.mutex};
    for (auto& policy : state.policies) {
        if (policy.tid == tid) {
            policy.group = group;
            return;
        }
    }
    state.policies.push_back(LinuxThreadPolicy{tid, group});
}

} // Anonymous namespace
#endif

void SetCurrentThreadPriority(ThreadPriority new_priority) {
#ifdef _WIN32
    int windows_priority = [&]() {
        switch (new_priority) {
        case ThreadPriority::Low: return THREAD_PRIORITY_BELOW_NORMAL;
        case ThreadPriority::Normal: return THREAD_PRIORITY_NORMAL;
        case ThreadPriority::High: return THREAD_PRIORITY_ABOVE_NORMAL;
        case ThreadPriority::VeryHigh: return THREAD_PRIORITY_HIGHEST;
        case ThreadPriority::Critical: return THREAD_PRIORITY_TIME_CRITICAL;
        default: return THREAD_PRIORITY_NORMAL;
        }
    }();
    SetThreadPriority(GetCurrentThread(), windows_priority);
#elif defined(__HAIKU__)
    // TODO: We have priorities for 3D rendering applications - may help lavapipe?
    int priority = [&]() {
        switch (new_priority) {
        case ThreadPriority::Low: return B_LOW_PRIORITY;
        case ThreadPriority::Normal: return B_NORMAL_PRIORITY;
        case ThreadPriority::High: return B_DISPLAY_PRIORITY;
        case ThreadPriority::VeryHigh: return B_URGENT_DISPLAY_PRIORITY;
        case ThreadPriority::Critical: return B_URGENT_PRIORITY;
        default: return B_NORMAL_PRIORITY;
        }
    }();
    set_thread_priority(find_thread(NULL), priority);
#elif defined(__ANDROID__)
    const int nice_value = NiceValueForPriority(new_priority);
    const pid_t tid = gettid();
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice_value) != 0) {
        LOG_WARNING(Common, "Could not set thread nice value to {}: {}", nice_value,
                    GetLastErrorMsg());
        return;
    }
    RememberCurrentThreadNice(tid, nice_value);
#elif defined(__linux__)
    const int nice_value = NiceValueForPriority(new_priority);
    if (setpriority(PRIO_PROCESS, 0, nice_value) != 0) {
        LOG_DEBUG(Common, "Could not set thread nice value to {}: {}", nice_value,
                  GetLastErrorMsg());
    }
#else
    const s32 max_prio = sched_get_priority_max(SCHED_OTHER);
    const s32 min_prio = sched_get_priority_min(SCHED_OTHER);
    if (max_prio > min_prio) {
        const u32 level = (std::min)(static_cast<u32>(new_priority), 4U);
        sched_param params{};
        params.sched_priority =
            min_prio + static_cast<s32>(static_cast<u32>(max_prio - min_prio) * level) / 4;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &params);
    }
#endif
}

void SetCurrentThreadName(const char* name) {
#ifdef _MSC_VER
    // Sets the debugger-visible name of the current thread.
    if (auto pf = (decltype(&SetThreadDescription))(void*)GetProcAddress(GetModuleHandle(TEXT("KernelBase.dll")), "SetThreadDescription"); pf)
        pf(GetCurrentThread(), UTF8ToUTF16W(name).data()); // Windows 10+
    else
        ; // No-op
#elif  defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__HAIKU__)
    rename_thread(find_thread(NULL), name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__) || defined(__CYGWIN__) || defined(__sun__) || defined(__glibc__) || defined(__managarm__)
    int ret = pthread_setname_np(pthread_self(), name);
    if (ret == ERANGE) {
        // Linux limits thread names to 15 characters and will outright reject any
        // attempt to set a longer name with ERANGE.
        char buf[16];
        size_t const len = std::min<size_t>(std::strlen(name), sizeof(buf) - 1);
        std::memcpy(buf, name, len);
        buf[len] = '\0';
        pthread_setname_np(pthread_self(), buf);
    }
#elif defined(_WIN32)
    // MinGW with the POSIX threading model does not support pthread_setname_np
    // See for reference
    // https://gitlab.freedesktop.org/mesa/mesa/-/blame/main/src/util/u_thread.c?ref_type=heads#L75
    (void)name;
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetCurrentThreadToPerformanceCores() {
#if defined(__ANDROID__)
    if (ADPF::AddCurrentThread(ADPF::Session::Render)) {
        SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
        return;
    }
    SetCurrentThreadCoreGroup(CoreGroup::Performance);
#elif defined(_WIN32)
    SetCurrentThreadWin32Group(Win32CoreGroup::Performance);
#elif defined(__linux__)
    SetCurrentThreadLinuxGroup(LinuxCoreGroup::Performance);
#endif
}

void SetCurrentThreadToEfficiencyCores() {
#if defined(__ANDROID__)
    if (ADPF::AddCurrentThread(ADPF::Session::Background)) {
        SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
        return;
    }
    SetCurrentThreadCoreGroup(CoreGroup::Efficiency);
#elif defined(_WIN32)
    SetCurrentThreadWin32Group(Win32CoreGroup::Efficiency);
#elif defined(__linux__)
    SetCurrentThreadLinuxGroup(LinuxCoreGroup::Efficiency);
#endif
}

void SetCurrentThreadToBackgroundWork() {
#if defined(__ANDROID__)
    ADPF::AddCurrentThread(ADPF::Session::Background);
    SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
#elif defined(_WIN32)
    SetCurrentThreadWin32Group(Win32CoreGroup::Efficiency);
#elif defined(__linux__)
    SetCurrentThreadLinuxGroup(LinuxCoreGroup::Efficiency);
#endif
}

void SetCurrentThreadToAllCores() {
#if defined(__ANDROID__)
    ADPF::RemoveCurrentThread();
    SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
#elif defined(_WIN32)
    SetCurrentThreadWin32Group(Win32CoreGroup::Unrestricted);
#elif defined(__linux__)
    SetCurrentThreadLinuxGroup(LinuxCoreGroup::Unrestricted);
#endif
}

void RefreshThreadPolicies() {
#if defined(__ANDROID__)
    CpuTopologyState& state = State();
    std::scoped_lock topology_lock{state.topology_mutex};
    RefreshTopologyLocked(state);

    std::scoped_lock policy_lock{state.policy_mutex};
    std::erase_if(state.policies, [&state](const ThreadPolicy& policy) {
        bool gone = false;
        if (policy.has_nice &&
            setpriority(PRIO_PROCESS, static_cast<id_t>(policy.tid), policy.nice_value) != 0 &&
            errno == ESRCH) {
            gone = true;
        }
        if (!gone) {
            ApplyCoreGroupLocked(state, policy.tid, policy.group, &gone);
        }
        return gone;
    });
    SnapshotCanaryLocked(state);
#elif defined(_WIN32)
    const auto& topology = GetWin32CacheTopology();
    if (!topology.asymmetric) {
        return;
    }
    const bool vcache_enabled = IsVCachePinningEnabled();
    Win32PolicyState& state = GetWin32PolicyState();
    std::scoped_lock lock{state.mutex};
    for (const auto& policy : state.policies) {
        if (policy.handle != nullptr) {
            SetThreadAffinityMask(policy.handle,
                                  MaskForWin32Policy(topology, policy, vcache_enabled));
        }
    }
#elif defined(__linux__)
    const auto& topology = GetLinuxCacheTopology();
    if (!topology.asymmetric) {
        return;
    }
    const bool vcache_enabled = IsVCachePinningEnabled();
    LinuxPolicyState& state = GetLinuxPolicyState();
    std::scoped_lock lock{state.mutex};
    for (const auto& policy : state.policies) {
        const auto& mask = MaskForLinuxGroup(topology, policy.group, vcache_enabled);
        sched_setaffinity(policy.tid, sizeof(mask), &mask);
    }
#endif
}

void PollThreadPolicies() {
#if defined(__ANDROID__)
    static const bool needed = !KernelPreservesRequestedAffinity();
    if (!needed) {
        return;
    }

    CpuTopologyState& state = State();
    if (!DueForPoll(state)) {
        return;
    }

    pid_t tid;
    cpu_set_t expected;
    {
        std::scoped_lock lock{state.topology_mutex};
        if (!state.canary_valid) {
            return;
        }
        tid = state.canary_tid;
        expected = state.canary_mask;
    }

    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(tid, sizeof(current), &current) == 0 &&
        std::memcmp(&current, &expected, sizeof(current)) == 0) {
        return;
    }
    RefreshThreadPolicies();
#endif
}

GpuPinMode GpuPinModeFromEnv() {
    static const GpuPinMode mode = [] {
        const char* env = std::getenv("EDEN_NX2_GPU_PIN");
        const std::string_view value = env != nullptr ? env : "";
        if (value == "separate") {
            return GpuPinMode::Separate;
        }
        if (value == "pair+worker") {
            return GpuPinMode::PairWorker;
        }
        if (value == "separate+worker") {
            return GpuPinMode::SeparateWorker;
        }
        if (value == "pair") {
            return GpuPinMode::Pair;
        }
        if (!value.empty()) {
            LOG_WARNING(Common, "gpu_core_pin: unknown EDEN_NX2_GPU_PIN={}, using separate",
                        value);
        }
        return GpuPinMode::Separate;
    }();
    return mode;
}

void ReserveGpuCores(GpuPinMode mode) {
#ifdef _WIN32
    auto& reservation = GetWin32GpuReservation();
    std::call_once(reservation.once, [&] { ReserveWin32GpuCores(reservation, mode); });
#else
    (void)mode;
#endif
}

void PinCurrentThreadToGpuSlot(u32 slot) {
#ifdef _WIN32
    auto& reservation = GetWin32GpuReservation();
    if (ReservedGpuMask() == 0 || slot >= reservation.slot_cpu.size() ||
        reservation.slot_cpu[slot] < 0) {
        return;
    }
    (void)&t_win32_policy_registration;
    const int cpu = reservation.slot_cpu[slot];
    const DWORD_PTR mask = DWORD_PTR{1} << cpu;
    GROUP_AFFINITY affinity{};
    affinity.Group = 0;
    affinity.Mask = mask;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
        LOG_WARNING(Common, "gpu_core_pin: pinning slot {} to CPU {} failed ({})", slot, cpu,
                    GetLastError());
        return;
    }
    // The process default CPU sets leave the reserved CPUs out: select this one for the thread.
    if (const ULONG cpu_set = reservation.slot_cpu_set[slot]; cpu_set != 0) {
        SetThreadSelectedCpuSets(GetCurrentThread(), &cpu_set, 1);
    }
    const DWORD tid = GetCurrentThreadId();
    {
        Win32PolicyState& state = GetWin32PolicyState();
        std::scoped_lock lock{state.mutex};
        bool found = false;
        for (auto& policy : state.policies) {
            if (policy.tid == tid) {
                policy.pinned_mask = mask;
                found = true;
                break;
            }
        }
        HANDLE dup_handle = nullptr;
        if (!found &&
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                            &dup_handle, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                            0)) {
            state.policies.push_back(
                Win32ThreadPolicy{tid, dup_handle, Win32CoreGroup::Performance, mask});
        }
    }
    LOG_INFO(Common, "gpu_core_pin: thread {} ({}) pinned to CPU {} (slot {}, mode {})",
             Win32ThreadName(GetCurrentThread()), tid, cpu, slot,
             GpuPinModeName(reservation.mode));
#else
    (void)slot;
#endif
}

u32 CurrentCpuNumber() {
#ifdef _WIN32
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return static_cast<u32>(number.Group) * 64 + number.Number;
#elif defined(__linux__)
    const int cpu = sched_getcpu();
    return cpu < 0 ? 0 : static_cast<u32>(cpu);
#else
    return 0;
#endif
}

u64 CurrentThreadAffinityMask() {
#ifdef _WIN32
    GROUP_AFFINITY affinity{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &affinity) || affinity.Group != 0) {
        return 0;
    }
    return static_cast<u64>(affinity.Mask);
#elif defined(__linux__) && !defined(__ANDROID__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return 0;
    }
    u64 mask = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            mask |= u64{1} << cpu;
        }
    }
    return mask;
#else
    return 0;
#endif
}

#ifdef ARCHITECTURE_x86_64
// On Linux and UNIX systems, a futex would nominally be used to cover the costs
// the idea is that it's intuitivelly cheaper to use a direct instruction as opposed to a full futex call
// the underlying libc++ implementation uses pthread_cond_timedwait which MAY invoke a futex
// Let's pretend the OS is too expensive to jump into, and avoid ANY context switches
// this should *IN THEORY* lower CPU usage while just waiting for stuff effectively
// For windows the minimal quanta resolution is about 500us, and normal CRT cond var is 1.5ms(?)
// so may as well avoid that too
// Let's just give ALL platforms the same mechanisms (almost) for when they have umonitor OR waitpkg
#ifdef __clang__
__attribute__((target("waitpkg,mwaitx")))
#elif defined(__GNUC__)
#pragma GCC target("waitpkg")
#pragma GCC target("mwaitx")
#endif
bool Event::WaitFor(const std::chrono::nanoseconds time) {
#ifdef _WIN32
    auto const& caps = Common::g_cpu_caps;
    const auto deadline = EventWaitDeadline(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()), time);
    const auto remaining = [deadline] {
        return deadline - std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch());
    };
    const u64 ticks_per_second = Common::g_wall_clock.NsToTicks(std::chrono::seconds{1});
    if (caps.monitorx) {
        while (true) {
            // Armed monitor, as per manual, MWAITX must be conditional if the condition isn't satisfied
            // to prevent a race condition.
            _mm_monitorx(reinterpret_cast<u64*>(std::addressof(is_set)), 0, 0);
            if (!is_set.load()) {
                const u32 cycles = MwaitxTimeoutCycles(remaining(), ticks_per_second);
                if (cycles != 0) {
                    // See here: https://github.com/torvalds/linux/blob/948a64995aca6820abefd17f1a4258f5835c5ad9/arch/x86/lib/delay.c#L93
                    // MWAITX accepts a 32-bit input timer which determines the total number of cycles to wait for
                    // NOT THE TOTAL ABSOLUTE TSC VALUE, it's just a delta
                    // BIT[1] = use a timer
                    // Hint = 0: Use C1 state when sleepy (means slower wakeup but better savings)
                    _mm_mwaitx(1 << 1, 0u, cycles);
                    continue;
                } else
                    return false; //timeout
            }
            bool expected = true;
            if (is_set.compare_exchange_weak(expected, false, std::memory_order_release))
                return true;
        }
    }
    if (caps.waitpkg) {
        // #UD If CPUID.7.0:ECX.WAITPKG[bit 5]=0.
        while (true) {
            _umonitor(std::addressof(is_set));
            if (!is_set.load()) {
                // UMWAIT also wakes early on interrupts, and its result (CF) only reports the OS
                // limit (IA32_UMWAIT_CONTROL). Wait again until the deadline, as for MWAITX. The
                // bounded budget keeps the absolute TSC deadline from wrapping.
                const u32 cycles = MwaitxTimeoutCycles(remaining(), ticks_per_second);
                if (cycles == 0)
                    return false; // timeout
                _umwait(0, Common::X64::FencedRDTSC() + cycles); // umwait is absolute time!!!
                continue;
            }
            bool expected = true;
            if (is_set.compare_exchange_weak(expected, false, std::memory_order_release))
                return true;
        }
    }
    while (!is_set.load() && remaining() > std::chrono::nanoseconds::zero())
        Common::Windows::SleepForOneTick();
    bool expected = true;
    return is_set.compare_exchange_strong(expected, false, std::memory_order_release);
#else
    std::unique_lock lk{mutex};
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::time_point{EventWaitDeadline(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()), time)};
    if (!condvar.wait_until(lk, deadline, [this] { return is_set.load(); }))
        return false;
    is_set = false;
    return true;
#endif
}
#else
bool Event::WaitFor(const std::chrono::nanoseconds time) {
#ifdef _WIN32
    auto const end = Common::g_wall_clock.GetTimeNS() + time;
    while (!is_set.load() && end > Common::g_wall_clock.GetTimeNS())
        Common::Windows::SleepForOneTick();
    if (is_set.load())
        Reset();
    return true;
#else
    std::unique_lock lk{mutex};
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::time_point{EventWaitDeadline(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()), time)};
    if (!condvar.wait_until(lk, deadline, [this] { return is_set.load(); }))
        return false;
    is_set = false;
    return true;
#endif
}
#endif

} // namespace Common
