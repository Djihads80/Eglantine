// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/heap_tracker.h"
#include "common/logging.h"
#include "core/arm/nce_stats.h"

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

namespace Core::NceStats {

namespace {

std::array<CoreCounters, MaxCores> g_cores;
SharedCounters g_shared;

double TicksPerSecond() {
#if defined(__aarch64__)
    u64 frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return static_cast<double>(frequency);
#else
    return 1e9;
#endif
}

const char* SvcName(u32 id) {
    switch (id) {
    case 0x01: return "SetHeapSize";
    case 0x02: return "SetMemoryPermission";
    case 0x03: return "SetMemoryAttribute";
    case 0x04: return "MapMemory";
    case 0x05: return "UnmapMemory";
    case 0x06: return "QueryMemory";
    case 0x08: return "CreateThread";
    case 0x09: return "StartThread";
    case 0x0a: return "ExitThread";
    case 0x0b: return "SleepThread";
    case 0x0c: return "GetThreadPriority";
    case 0x0d: return "SetThreadPriority";
    case 0x0e: return "GetThreadCoreMask";
    case 0x0f: return "SetThreadCoreMask";
    case 0x10: return "GetCurrentProcessorNumber";
    case 0x11: return "SignalEvent";
    case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory";
    case 0x16: return "CloseHandle";
    case 0x17: return "ResetSignal";
    case 0x18: return "WaitSynchronization";
    case 0x19: return "CancelSynchronization";
    case 0x1a: return "ArbitrateLock";
    case 0x1b: return "ArbitrateUnlock";
    case 0x1c: return "WaitProcessWideKeyAtomic";
    case 0x1d: return "SignalProcessWideKey";
    case 0x1e: return "GetSystemTick";
    case 0x1f: return "ConnectToNamedPort";
    case 0x20: return "SendSyncRequestLight";
    case 0x21: return "SendSyncRequest";
    case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x23: return "SendAsyncRequestWithUserBuffer";
    case 0x24: return "GetProcessId";
    case 0x25: return "GetThreadId";
    case 0x26: return "Break";
    case 0x27: return "OutputDebugString";
    case 0x29: return "GetInfo";
    case 0x2a: return "FlushEntireDataCache";
    case 0x2b: return "FlushDataCache";
    case 0x2c: return "MapPhysicalMemory";
    case 0x2d: return "UnmapPhysicalMemory";
    case 0x32: return "SetThreadActivity";
    case 0x33: return "GetThreadContext3";
    case 0x34: return "WaitForAddress";
    case 0x35: return "SignalToAddress";
    case 0x36: return "SynchronizePreemptionState";
    case 0x40: return "CreateSession";
    case 0x41: return "AcceptSession";
    case 0x43: return "ReplyAndReceive";
    case 0x44: return "ReplyAndReceiveWithUserBuffer";
    case 0x45: return "CreateEvent";
    case 0x4b: return "CreateCodeMemory";
    case 0x4c: return "ControlCodeMemory";
    case 0x51: return "MapTransferMemory";
    case 0x52: return "UnmapTransferMemory";
    case 0x5d: return "InvalidateProcessDataCache";
    case 0x5e: return "StoreProcessDataCache";
    case 0x5f: return "FlushProcessDataCache";
    case 0x6f: return "GetSystemInfo";
    case 0x73: return "SetProcessMemoryPermission";
    case 0x74: return "MapProcessMemory";
    case 0x77: return "MapProcessCodeMemory";
    default: return "?";
    }
}

struct CoreSnapshot {
    std::array<u64, static_cast<size_t>(State::Count)> state_ticks{};
    std::array<u64, static_cast<size_t>(Fault::Count)> fault_count{};
    std::array<u64, static_cast<size_t>(Fault::Count)> fault_ticks{};
    u64 runs{}, signal_entries{}, svc_exits{}, interrupt_exits{};
    u64 idle_calls{}, idle_wakes{}, wake_ticks{};
    u64 interrupt_signals{}, interrupt_idle{};
    std::array<u64, SvcSlots> svc_count{};
    std::array<u64, SvcSlots> svc_ticks{};
};

template <size_t N>
void Copy(std::array<u64, N>& out, const std::array<std::atomic<u64>, N>& in) {
    for (size_t i = 0; i < N; ++i) {
        out[i] = in[i].load(std::memory_order_relaxed);
    }
}

CoreSnapshot Take(const CoreCounters& c) {
    CoreSnapshot s;
    Copy(s.state_ticks, c.state_ticks);
    Copy(s.fault_count, c.fault_count);
    Copy(s.fault_ticks, c.fault_ticks);
    Copy(s.svc_count, c.svc_count);
    Copy(s.svc_ticks, c.svc_ticks);
    s.runs = c.runs.load(std::memory_order_relaxed);
    s.signal_entries = c.signal_entries.load(std::memory_order_relaxed);
    s.svc_exits = c.svc_exits.load(std::memory_order_relaxed);
    s.interrupt_exits = c.interrupt_exits.load(std::memory_order_relaxed);
    s.idle_calls = c.idle_calls.load(std::memory_order_relaxed);
    s.idle_wakes = c.idle_wakes.load(std::memory_order_relaxed);
    s.wake_ticks = c.wake_ticks.load(std::memory_order_relaxed);
    s.interrupt_signals = c.interrupt_signals.load(std::memory_order_relaxed);
    s.interrupt_idle = c.interrupt_idle.load(std::memory_order_relaxed);
    return s;
}

struct SharedSnapshot {
    u64 cache_protects{}, cache_protect_bytes{}, cache_protect_ticks{};
    u64 gpu_invalidate_fast{}, gpu_invalidate_sync{}, gpu_invalidate_sync_ticks{};
    u64 heap_protects{}, heap_deferred_maps{}, heap_rebuilds{}, heap_evictions{};
};

SharedSnapshot TakeShared() {
    const auto& heap = Common::GetHeapTrackerCounters();
    return SharedSnapshot{
        .cache_protects = g_shared.cache_protects.load(std::memory_order_relaxed),
        .cache_protect_bytes = g_shared.cache_protect_bytes.load(std::memory_order_relaxed),
        .cache_protect_ticks = g_shared.cache_protect_ticks.load(std::memory_order_relaxed),
        .gpu_invalidate_fast = g_shared.gpu_invalidate_fast.load(std::memory_order_relaxed),
        .gpu_invalidate_sync = g_shared.gpu_invalidate_sync.load(std::memory_order_relaxed),
        .gpu_invalidate_sync_ticks =
            g_shared.gpu_invalidate_sync_ticks.load(std::memory_order_relaxed),
        .heap_protects = heap.protects.load(std::memory_order_relaxed),
        .heap_deferred_maps = heap.deferred_maps.load(std::memory_order_relaxed),
        .heap_rebuilds = heap.rebuilds.load(std::memory_order_relaxed),
        .heap_evictions = heap.evictions.load(std::memory_order_relaxed),
    };
}

// ---- Host threads: CPU time and run-queue wait, from /proc ----

struct ThreadSample {
    std::string name;
    u64 run_ns{};
    u64 wait_ns{};
    int cpu{-1};
    bool has_wait{};
};

#ifdef __linux__
bool ReadSmallFile(const std::string& path, char* buffer, size_t size) {
    FILE* file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
        return false;
    }
    const size_t length = std::fread(buffer, 1, size - 1, file);
    std::fclose(file);
    buffer[length] = '\0';
    return length > 0;
}

std::unordered_map<long, ThreadSample> ReadThreads() {
    std::unordered_map<long, ThreadSample> threads;
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return threads;
    }
    static const double ns_per_clock_tick = 1e9 / static_cast<double>(sysconf(_SC_CLK_TCK));
    char buffer[1024];
    while (const dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long tid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0') {
            continue;
        }
        const std::string base = std::string{"/proc/self/task/"} + entry->d_name + "/";
        ThreadSample sample;
        if (ReadSmallFile(base + "comm", buffer, sizeof(buffer))) {
            sample.name = buffer;
            while (!sample.name.empty() && sample.name.back() == '\n') {
                sample.name.pop_back();
            }
        }
        if (!ReadSmallFile(base + "stat", buffer, sizeof(buffer))) {
            continue;
        }
        // The fields after the name: state is field 3, utime 14, stime 15, processor 39.
        const char* fields = std::strrchr(buffer, ')');
        if (fields == nullptr) {
            continue;
        }
        std::vector<unsigned long long> values;
        for (const char* p = fields + 2; *p != '\0';) {
            while (*p == ' ') {
                ++p;
            }
            char* next = nullptr;
            const unsigned long long value = std::strtoull(p, &next, 10);
            if (next == p) {
                // The state letter.
                values.push_back(0);
                while (*p != '\0' && *p != ' ') {
                    ++p;
                }
                continue;
            }
            values.push_back(value);
            p = next;
        }
        if (values.size() > 36) {
            sample.cpu = static_cast<int>(values[36]);
        }
        unsigned long long run = 0, wait = 0, slices = 0;
        if (ReadSmallFile(base + "schedstat", buffer, sizeof(buffer)) &&
            std::sscanf(buffer, "%llu %llu %llu", &run, &wait, &slices) >= 2) {
            sample.run_ns = run;
            sample.wait_ns = wait;
            sample.has_wait = true;
        } else if (values.size() > 12) {
            sample.run_ns =
                static_cast<u64>(static_cast<double>(values[11] + values[12]) * ns_per_clock_tick);
        }
        threads.emplace(tid, std::move(sample));
    }
    closedir(dir);
    return threads;
}
#else
std::unordered_map<long, ThreadSample> ReadThreads() {
    return {};
}
#endif

void Report(double seconds, double frequency, const std::vector<CoreSnapshot>& now,
            const std::vector<CoreSnapshot>& before, const SharedSnapshot& shared_now,
            const SharedSnapshot& shared_before) {
    const double window = seconds * frequency;
    const auto percent = [&](u64 ticks) { return 100.0 * static_cast<double>(ticks) / window; };
    const auto per_second = [&](u64 count) { return static_cast<double>(count) / seconds; };
    const auto micros = [&](u64 ticks, u64 count) {
        return count == 0 ? 0.0 : 1e6 * static_cast<double>(ticks) / frequency / count;
    };

    std::array<u64, SvcSlots> svc_count{};
    std::array<u64, SvcSlots> svc_ticks{};
    for (size_t core = 0; core < MaxCores; ++core) {
        const CoreSnapshot& a = now[core];
        const CoreSnapshot& b = before[core];
        const auto d = [](u64 x, u64 y) { return x - y; };
        u64 state_total = 0;
        for (size_t i = 0; i < a.state_ticks.size(); ++i) {
            state_total += d(a.state_ticks[i], b.state_ticks[i]);
        }
        u64 fault_ticks = 0;
        u64 fault_count = 0;
        for (size_t i = 0; i < a.fault_count.size(); ++i) {
            fault_ticks += d(a.fault_ticks[i], b.fault_ticks[i]);
            fault_count += d(a.fault_count[i], b.fault_count[i]);
        }
        for (size_t i = 0; i < SvcSlots; ++i) {
            svc_count[i] += d(a.svc_count[i], b.svc_count[i]);
            svc_ticks[i] += d(a.svc_ticks[i], b.svc_ticks[i]);
        }
        if (state_total == 0 && d(a.interrupt_idle, b.interrupt_idle) == 0) {
            continue;
        }
        const auto state = [&](State s) {
            const size_t i = static_cast<size_t>(s);
            return d(a.state_ticks[i], b.state_ticks[i]);
        };
        const u64 wakes = d(a.idle_wakes, b.idle_wakes);
        LOG_INFO(Core_ARM,
                 "NCE stats core {}: guest {:.1f}% (of which fault handlers {:.2f}%), SVCs {:.1f}%, "
                 "other host {:.1f}%, idle {:.1f}% | guest entries {:.0f}/s ({:.0f}/s by signal), "
                 "exits: SVC {:.0f}/s, interrupt {:.0f}/s | idle calls {:.0f}/s, slept {:.0f}/s, "
                 "wake-up {:.1f} us avg {:.1f} us max | interrupts sent: running {:.0f}/s, "
                 "not running {:.0f}/s",
                 core, percent(state(State::Guest)), percent(fault_ticks),
                 percent(state(State::Svc)), percent(state(State::Host)),
                 percent(state(State::Idle)), per_second(d(a.runs, b.runs)),
                 per_second(d(a.signal_entries, b.signal_entries)),
                 per_second(d(a.svc_exits, b.svc_exits)),
                 per_second(d(a.interrupt_exits, b.interrupt_exits)),
                 per_second(d(a.idle_calls, b.idle_calls)), per_second(wakes),
                 micros(d(a.wake_ticks, b.wake_ticks), wakes),
                 micros(g_cores[core].wake_max_ticks.exchange(0, std::memory_order_relaxed), 1),
                 per_second(d(a.interrupt_signals, b.interrupt_signals)),
                 per_second(d(a.interrupt_idle, b.interrupt_idle)));
        if (fault_count != 0) {
            static constexpr std::array<const char*, static_cast<size_t>(Fault::Count)> names{
                "GPU-cached write", "separate heap", "mapped", "unhandled", "alignment"};
            std::string text;
            for (size_t i = 0; i < names.size(); ++i) {
                const u64 count = d(a.fault_count[i], b.fault_count[i]);
                if (count == 0) {
                    continue;
                }
                text += fmt::format(" | {} {:.1f}/s, {:.1f} us avg", names[i], per_second(count),
                                    micros(d(a.fault_ticks[i], b.fault_ticks[i]), count));
            }
            LOG_INFO(Core_ARM, "NCE faults core {}: {:.1f}/s, {:.2f} ms max{}", core,
                     per_second(fault_count),
                     micros(g_cores[core].fault_max_ticks.exchange(0, std::memory_order_relaxed),
                            1) /
                         1000.0,
                     text);
        }
    }

    std::vector<size_t> order;
    for (size_t i = 0; i < SvcSlots; ++i) {
        if (svc_count[i] != 0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(),
              [&](size_t x, size_t y) { return svc_ticks[x] > svc_ticks[y]; });
    if (!order.empty()) {
        u64 total = 0;
        for (const u64 count : svc_count) {
            total += count;
        }
        std::string text;
        for (size_t i = 0; i < std::min<size_t>(order.size(), 14); ++i) {
            const size_t id = order[i];
            text += fmt::format(" | {:#04x} {} {:.0f}/s {:.1f} us", id, SvcName(static_cast<u32>(id)),
                                per_second(svc_count[id]), micros(svc_ticks[id], svc_count[id]));
        }
        LOG_INFO(Core_ARM, "NCE SVCs: {:.0f}/s (host time each, by total){}", per_second(total),
                 text);
    }

    const auto& heap = Common::GetHeapTrackerCounters();
    const u64 protects = shared_now.cache_protects - shared_before.cache_protects;
    const u64 syncs = shared_now.gpu_invalidate_sync - shared_before.gpu_invalidate_sync;
    LOG_INFO(Core_ARM,
             "NCE memory: GPU cache protection changes {:.0f}/s ({:.1f} MiB/s, {:.2f} ms/s) | CPU "
             "write invalidations {:.0f}/s in place, {:.1f}/s waited for the GPU thread ({:.1f} "
             "us avg) | heap tracker: protects {:.0f}/s, maps {} ({} resident), separate heap "
             "maps back in {:.1f}/s, rebuilds {} ({} evicted)",
             per_second(protects),
             per_second(shared_now.cache_protect_bytes - shared_before.cache_protect_bytes) /
                 (1024.0 * 1024.0),
             1e3 * static_cast<double>(shared_now.cache_protect_ticks -
                                       shared_before.cache_protect_ticks) /
                 frequency / seconds,
             per_second(shared_now.gpu_invalidate_fast - shared_before.gpu_invalidate_fast),
             per_second(syncs),
             micros(shared_now.gpu_invalidate_sync_ticks - shared_before.gpu_invalidate_sync_ticks,
                    syncs),
             per_second(shared_now.heap_protects - shared_before.heap_protects),
             heap.map_count.load(std::memory_order_relaxed),
             heap.resident_map_count.load(std::memory_order_relaxed),
             per_second(shared_now.heap_deferred_maps - shared_before.heap_deferred_maps),
             shared_now.heap_rebuilds - shared_before.heap_rebuilds,
             shared_now.heap_evictions - shared_before.heap_evictions);
}

void ReportThreads(double seconds, const std::unordered_map<long, ThreadSample>& now,
                   const std::unordered_map<long, ThreadSample>& before) {
    struct Row {
        long tid;
        const ThreadSample* sample;
        u64 run;
        u64 wait;
    };
    std::vector<Row> rows;
    u64 total_run = 0;
    for (const auto& [tid, sample] : now) {
        const auto it = before.find(tid);
        const u64 run = it == before.end() ? sample.run_ns : sample.run_ns - it->second.run_ns;
        const u64 wait = it == before.end() ? sample.wait_ns : sample.wait_ns - it->second.wait_ns;
        total_run += run;
        rows.push_back({tid, &sample, run, wait});
    }
    if (rows.empty()) {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.run > y.run; });
    const double window_ns = seconds * 1e9;
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 16); ++i) {
        const Row& row = rows[i];
        if (row.run < window_ns / 200) {
            break;
        }
        text += fmt::format(" | {} {:.0f}%", row.sample->name, 100.0 * row.run / window_ns);
        if (row.sample->has_wait) {
            text += fmt::format(" wait {:.0f}%", 100.0 * row.wait / window_ns);
        }
        text += fmt::format(" cpu{}", row.sample->cpu);
    }
    LOG_INFO(Core_ARM, "NCE threads: process {:.0f}% of a CPU over {} threads (CPU time, run-queue "
                       "wait, last CPU){}",
             100.0 * total_run / window_ns, rows.size(), text);
}

void ReporterLoop() {
    using namespace std::chrono_literals;
    const double frequency = TicksPerSecond();
    // Two snapshots of every core's counters are 100 KiB: not on this thread's stack.
    std::vector<CoreSnapshot> before(MaxCores);
    std::vector<CoreSnapshot> now(MaxCores);
    for (size_t core = 0; core < MaxCores; ++core) {
        before[core] = Take(g_cores[core]);
    }
    SharedSnapshot shared_before = TakeShared();
    auto threads_before = ReadThreads();
    auto time_before = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(10s);
        const auto time_now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(time_now - time_before).count();
        for (size_t core = 0; core < MaxCores; ++core) {
            now[core] = Take(g_cores[core]);
        }
        const SharedSnapshot shared_now = TakeShared();
        auto threads_now = ReadThreads();
        Report(seconds, frequency, now, before, shared_now, shared_before);
        ReportThreads(seconds, threads_now, threads_before);
        std::swap(before, now);
        shared_before = shared_now;
        threads_before = std::move(threads_now);
        time_before = time_now;
    }
}

} // namespace

CoreCounters& Counters(size_t core) {
    return g_cores[core];
}

SharedCounters& Shared() {
    return g_shared;
}

void Init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* value = std::getenv("EDEN_NCE_STATS");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') {
            return;
        }
        g_enabled.store(true, std::memory_order_relaxed);
        std::thread{ReporterLoop}.detach();
        LOG_INFO(Core_ARM, "NCE stats every 10 s (EDEN_NCE_STATS)");
    });
}

} // namespace Core::NceStats
