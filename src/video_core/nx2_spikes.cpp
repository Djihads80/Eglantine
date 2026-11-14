// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>

#include "common/log_ring.h"
#include "common/logging.h"
#include "common/thread.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_spikes.h"

#ifdef __linux__
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace VideoCommon::Nx2Spikes {
namespace {

namespace Profile = Nx2Profile;

constexpr size_t MedianFrames = 120;
constexpr size_t MinFramesForMedian = 30;
constexpr size_t RecentSpikes = 32;
constexpr size_t WorstSpikes = 8;
constexpr size_t LogLinesPerSpike = 30;
constexpr size_t ZonesPerThread = 8;
constexpr size_t HostThreadsPerSpike = 16;
constexpr size_t RingLines = 20000;

constexpr size_t NumZones = static_cast<size_t>(Profile::Zone::Count);
constexpr size_t NumCounters = std::tuple_size_v<decltype(Profile::Totals::counters)>;
constexpr size_t NumEvents = static_cast<size_t>(Event::Count);

constexpr std::array<const char*, NumCounters> counter_names{
    "download_bytes",  "download_copies", "mme_dma_read_bytes", "download_bytes_on_dma_pages",
    "method_writes",   "same_value_writes", "acquire_checks",   "acquire_waits",
    "dma_reads_drained", "dma_reads_skipped"};
constexpr std::array<const char*, NumEvents> event_names{
    "driver pipeline builds",
    "driver compiles (over 10 ms)",
    "pipelines built on the GPU thread",
    "draws skipped for translation",
    "CPU writes to GPU-cached pages",
    "CPU writes reaching the GPU caches",
    "CPU write waits for a GPU cache lock",
    "page protection changes",
    "read traps hit",
    "JIT blocks compiled",
    "JIT fastmem recompiles",
    "JIT invalidations",
    "JIT cache clears",
    "SVCs",
    "SVC sleep/yield",
    "SVC WaitSynchronization",
    "SVC mutex/condvar",
    "SVC address arbiter",
    "SVC IPC"};
constexpr std::array<const char*, NumEvents> event_units{
    nullptr, nullptr, nullptr, nullptr, "bytes", "bytes", nullptr, "pages", nullptr, nullptr,
    nullptr, "bytes", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
constexpr size_t GuestThreadsPerSpike = 8;

double Ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::atomic<bool> g_enabled{};
/// The recorder and open Windows: events are counted while above zero.
std::atomic<int> g_counting{};

// Events are counted in per-thread shards: CPU write events come from every guest core at once.
struct alignas(64) Shard {
    std::array<std::atomic<u64>, NumEvents> count{};
    std::array<std::atomic<u64>, NumEvents> ns{};
    std::array<std::atomic<u64>, NumEvents> amount{};
};
constexpr size_t NumShards = 16;
std::array<Shard, NumShards> g_shards{};

Shard& ThisThreadShard() {
    static std::atomic<u32> next{};
    thread_local const u32 index = next.fetch_add(1, std::memory_order_relaxed) % NumShards;
    return g_shards[index];
}

struct Polled {
    std::atomic<u64 (*)()> count{};
    std::atomic<u64 (*)()> ns{};
};
std::array<Polled, NumEvents> g_polled{};

struct EventTotals {
    std::array<u64, NumEvents> count{};
    std::array<u64, NumEvents> ns{};
    std::array<u64, NumEvents> amount{};
};

EventTotals ReadEvents() {
    EventTotals totals;
    for (const Shard& shard : g_shards) {
        for (size_t i = 0; i < NumEvents; ++i) {
            totals.count[i] += shard.count[i].load(std::memory_order_relaxed);
            totals.ns[i] += shard.ns[i].load(std::memory_order_relaxed);
            totals.amount[i] += shard.amount[i].load(std::memory_order_relaxed);
        }
    }
    for (size_t i = 0; i < NumEvents; ++i) {
        if (const auto count = g_polled[i].count.load(std::memory_order_relaxed)) {
            totals.count[i] += count();
        }
        if (const auto ns = g_polled[i].ns.load(std::memory_order_relaxed)) {
            totals.ns[i] += ns();
        }
    }
    return totals;
}

std::vector<EventTimes> EventDifference(const EventTotals& before, const EventTotals& after) {
    std::vector<EventTimes> result;
    for (size_t i = 0; i < NumEvents; ++i) {
        const u64 count = after.count[i] - before.count[i];
        if (count != 0) {
            result.push_back({event_names[i], count,
                              static_cast<double>(after.ns[i] - before.ns[i]) / 1e6,
                              after.amount[i] - before.amount[i], event_units[i]});
        }
    }
    return result;
}

// Guest thread CPU times, read from the core's source.
std::mutex g_guest_mutex;
GuestThreadSource g_guest_source{};

struct GuestSnapshot {
    bool valid{};
    Clock::time_point when;
    u64 clock{};
    std::vector<GuestThreadTicks> threads;
};

GuestSnapshot TakeGuestSnapshot() {
    GuestSnapshot snap;
    std::scoped_lock lock{g_guest_mutex};
    if (g_guest_source.read == nullptr) {
        return snap;
    }
    snap.when = Clock::now();
    snap.valid = g_guest_source.read(g_guest_source.user, snap.threads, snap.clock);
    return snap;
}

/// The busiest guest threads between two snapshots, in host ms (the guest clock is scaled by
/// how far it moved against wall time).
std::vector<GuestThreadTimes> GuestDifference(const GuestSnapshot& before,
                                              const GuestSnapshot& after, size_t limit) {
    std::vector<GuestThreadTimes> result;
    if (!before.valid || !after.valid || after.clock <= before.clock || after.when <= before.when) {
        return result;
    }
    const double ms_per_tick =
        Ms(after.when - before.when) / static_cast<double>(after.clock - before.clock);
    std::vector<std::pair<u64, const GuestThreadTicks*>> busy;
    for (const GuestThreadTicks& now : after.threads) {
        const auto it = std::find_if(before.threads.begin(), before.threads.end(),
                                     [&](const GuestThreadTicks& t) { return t.id == now.id; });
        const u64 old = it != before.threads.end() ? it->ticks : 0;
        if (now.ticks > old) {
            busy.emplace_back(now.ticks - old, &now);
        }
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (busy.size() > limit) {
        busy.resize(limit);
    }
    std::scoped_lock lock{g_guest_mutex};
    for (const auto& [ticks, thread] : busy) {
        const double ms = static_cast<double>(ticks) * ms_per_tick;
        if (ms < 0.05) {
            continue;
        }
        std::string name = g_guest_source.name != nullptr
                               ? g_guest_source.name(g_guest_source.user, thread->id)
                               : std::string{};
        result.push_back({thread->id, std::move(name), thread->core, ms});
    }
    return result;
}

struct GpuSnapshot {
    std::string thread;
    std::array<u64, NumZones> ns{};
    std::array<u64, NumZones> count{};
    std::array<u64, NumCounters> counters{};
};

std::vector<GpuSnapshot> TakeGpuSnapshot() {
    std::vector<GpuSnapshot> result;
    std::scoped_lock lock{Profile::PublishedMutex()};
    for (const auto& published : Profile::PublishedThreads()) {
        GpuSnapshot& snap = result.emplace_back();
        snap.thread = published.thread;
        for (size_t i = 0; i < NumZones; ++i) {
            snap.ns[i] = published.exclusive_ns[i].load(std::memory_order_relaxed);
            snap.count[i] = published.count[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < NumCounters; ++i) {
            snap.counters[i] = published.counters[i].load(std::memory_order_relaxed);
        }
    }
    return result;
}

std::vector<GpuThreadTimes> GpuDifference(const std::vector<GpuSnapshot>& before,
                                          const std::vector<GpuSnapshot>& after,
                                          Clock::duration frame) {
    std::vector<GpuThreadTimes> result;
    for (const GpuSnapshot& now : after) {
        const auto it = std::find_if(before.begin(), before.end(),
                                     [&](const GpuSnapshot& s) { return s.thread == now.thread; });
        if (it == before.end()) {
            continue;
        }
        GpuThreadTimes& times = result.emplace_back();
        times.thread = now.thread;
        u64 zoned{};
        for (size_t i = 1; i < NumZones; ++i) {
            const u64 ns = now.ns[i] - it->ns[i];
            const u64 calls = now.count[i] - it->count[i];
            zoned += ns;
            if (ns >= 100'000 || (calls != 0 && ns != 0)) {
                times.zones.push_back({Profile::zone_names[i], static_cast<double>(ns) / 1e6, calls});
            }
        }
        const u64 frame_ns = static_cast<u64>(std::chrono::nanoseconds{frame}.count());
        if (frame_ns > zoned) {
            times.zones.push_back({"other (no zone)", static_cast<double>(frame_ns - zoned) / 1e6, 0});
        }
        std::sort(times.zones.begin(), times.zones.end(),
                  [](const ZoneTime& a, const ZoneTime& b) { return a.ms > b.ms; });
        if (times.zones.size() > ZonesPerThread) {
            times.zones.resize(ZonesPerThread);
        }
        for (size_t i = 0; i < NumCounters; ++i) {
            if (const u64 value = now.counters[i] - it->counters[i]; value != 0) {
                times.counters.emplace_back(counter_names[i], value);
            }
        }
    }
    return result;
}

// Host thread times: Linux's per-thread scheduler statistics (time on a CPU and time runnable
// but waiting for one, in ns), sampled every 20 ms on a thread of their own.
struct TaskTimes {
    int tid{};
    u64 run_ns{};
    u64 wait_ns{};
};

struct HostSample {
    Clock::time_point when;
    std::vector<TaskTimes> tasks;
    u64 minor_faults{};
    u64 major_faults{};
    u64 read_bytes{};
};

#ifdef __linux__
std::optional<std::string> ReadSmallFile(const char* path) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    std::array<char, 1024> buffer;
    const ssize_t size = ::read(fd, buffer.data(), buffer.size() - 1);
    ::close(fd);
    if (size <= 0) {
        return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<size_t>(size));
}

HostSample TakeHostSample() {
    HostSample sample;
    sample.when = Clock::now();
    if (DIR* const dir = ::opendir("/proc/self/task")) {
        while (const dirent* const entry = ::readdir(dir)) {
            const int tid = std::atoi(entry->d_name);
            if (tid <= 0) {
                continue;
            }
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
            const auto text = ReadSmallFile(path);
            unsigned long long run = 0;
            unsigned long long wait = 0;
            if (text && std::sscanf(text->c_str(), "%llu %llu", &run, &wait) == 2) {
                sample.tasks.push_back({tid, run, wait});
            }
        }
        ::closedir(dir);
    }
    if (const auto stat = ReadSmallFile("/proc/self/stat")) {
        // Fields after the command name, which may hold spaces: state is field 3, minflt 10,
        // majflt 12.
        if (const size_t close = stat->rfind(')'); close != std::string::npos) {
            unsigned long long minflt = 0;
            unsigned long long majflt = 0;
            if (std::sscanf(stat->c_str() + close + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu",
                            &minflt, &majflt) == 2) {
                sample.minor_faults = minflt;
                sample.major_faults = majflt;
            }
        }
    }
    if (const auto io = ReadSmallFile("/proc/self/io")) {
        if (const size_t at = io->find("read_bytes:"); at != std::string::npos) {
            sample.read_bytes = std::strtoull(io->c_str() + at + 11, nullptr, 10);
        }
    }
    return sample;
}

std::string ThreadName(int tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    std::string name = ReadSmallFile(path).value_or("?");
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return name;
}
#endif

struct PendingSpike {
    Spike spike;
    Clock::time_point start;
    Clock::time_point end;
};

struct State {
    std::mutex mutex;
    double min_ms{50.0};
    double ratio{2.0};
    Clock::time_point started;
    bool has_last{};
    Clock::time_point last_entered;
    Clock::time_point last_paced;
    /// The previous present's parts, which fall inside the frame that ends at this one.
    Clock::duration last_fence{};
    Clock::duration last_composite{};
    Clock::duration last_pace{};
    std::deque<double> frames;
    double median_ms{};
    u64 presents{};
    std::vector<GpuSnapshot> last_gpu;
    EventTotals last_events;
    GuestSnapshot last_guest;
    u64 last_log_sequence{};
    std::vector<PendingSpike> pending;
    std::deque<Spike> recent;
    std::vector<Spike> worst;
    u64 spikes{};
    Clock::time_point log_window;
    u32 logged_in_window{};
    bool turned_profiler_on{};
    bool turned_ring_on{};
};

State& GetState() {
    static State state;
    return state;
}

struct Sampler {
    std::mutex mutex;
    std::deque<HostSample> samples; ///< The last 6 s
    std::unordered_map<int, std::string> names;
    std::jthread thread;
};

Sampler& GetSampler() {
    static Sampler sampler;
    return sampler;
}

#ifdef __linux__
/// Host thread times between two samples: the busiest `limit` threads, plus any that waited long
/// for a CPU. `names` caches thread names by tid.
std::vector<HostThreadTimes> HostDifference(const HostSample& before, const HostSample& after,
                                            std::unordered_map<int, std::string>& names,
                                            size_t limit) {
    std::vector<HostThreadTimes> host;
    for (const TaskTimes& task : after.tasks) {
        const auto it = std::find_if(before.tasks.begin(), before.tasks.end(),
                                     [&](const TaskTimes& t) { return t.tid == task.tid; });
        const u64 run = task.run_ns - (it != before.tasks.end() ? it->run_ns : 0);
        const u64 wait = task.wait_ns - (it != before.tasks.end() ? it->wait_ns : 0);
        if (run < 500'000 && wait < 500'000) {
            continue;
        }
        auto [name, inserted] = names.try_emplace(task.tid);
        if (inserted) {
            name->second = ThreadName(task.tid);
        }
        host.push_back({name->second, task.tid, static_cast<double>(run) / 1e6,
                        static_cast<double>(wait) / 1e6});
    }
    std::sort(host.begin(), host.end(),
              [](const HostThreadTimes& a, const HostThreadTimes& b) { return a.cpu_ms > b.cpu_ms; });
    if (host.size() > limit) {
        // Keep the threads that waited long for a CPU even when they ran little.
        const double long_wait = 5.0 * std::max(1.0, Ms(after.when - before.when) / 100.0);
        std::stable_partition(host.begin() + limit, host.end(),
                              [&](const HostThreadTimes& t) { return t.run_queue_ms >= long_wait; });
        const auto waited_end =
            std::find_if(host.begin() + limit, host.end(),
                         [&](const HostThreadTimes& t) { return t.run_queue_ms < long_wait; });
        host.erase(waited_end, host.end());
    }
    return host;
}
#endif

void FillHost(Spike& spike, Clock::time_point start, Clock::time_point end) {
#ifdef __linux__
    auto& sampler = GetSampler();
    std::scoped_lock lock{sampler.mutex};
    const HostSample* before = nullptr;
    const HostSample* after = nullptr;
    for (const HostSample& sample : sampler.samples) {
        if (sample.when <= start) {
            before = &sample;
        } else if (sample.when >= end && after == nullptr) {
            after = &sample;
        }
    }
    if (before == nullptr || after == nullptr) {
        return;
    }
    spike.host_sampled = true;
    spike.host_span_ms = Ms(after->when - before->when);
    spike.minor_faults = after->minor_faults - before->minor_faults;
    spike.major_faults = after->major_faults - before->major_faults;
    spike.read_kib = (after->read_bytes - before->read_bytes) / 1024;
    spike.host = HostDifference(*before, *after, sampler.names, HostThreadsPerSpike);
#else
    static_cast<void>(spike);
    static_cast<void>(start);
    static_cast<void>(end);
#endif
}

/// Records a finished spike (state mutex held).
void Finish(State& state, Spike spike) {
    const auto now = Clock::now();
    if (now - state.log_window > std::chrono::seconds{1}) {
        state.log_window = now;
        state.logged_in_window = 0;
    }
    if (++state.logged_in_window <= 4) {
        LOG_INFO(HW_GPU, "{}", Summary(spike));
    }
    state.recent.push_front(spike);
    if (state.recent.size() > RecentSpikes) {
        state.recent.pop_back();
    }
    state.worst.push_back(std::move(spike));
    std::sort(state.worst.begin(), state.worst.end(),
              [](const Spike& a, const Spike& b) { return a.frame_ms > b.frame_ms; });
    if (state.worst.size() > WorstSpikes) {
        state.worst.resize(WorstSpikes);
    }
}

/// Finishes the pending spikes the host samples now cover (state mutex held). `all` finishes
/// every one, with host times or without.
void FinishPending(State& state, std::optional<Clock::time_point> covered, bool all) {
    auto& pending = state.pending;
    for (auto it = pending.begin(); it != pending.end();) {
        const bool ready = covered && *covered >= it->end;
        if (!ready && !all) {
            ++it;
            continue;
        }
        if (ready) {
            FillHost(it->spike, it->start, it->end);
        }
        Finish(state, std::move(it->spike));
        it = pending.erase(it);
    }
}

#ifdef __linux__
void SamplerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("Nx2SpikeSampler");
    auto& sampler = GetSampler();
    while (!stop.stop_requested()) {
        HostSample sample = TakeHostSample();
        const auto when = sample.when;
        {
            std::scoped_lock lock{sampler.mutex};
            sampler.samples.push_back(std::move(sample));
            while (!sampler.samples.empty() &&
                   when - sampler.samples.front().when > std::chrono::seconds{6}) {
                sampler.samples.pop_front();
            }
        }
        {
            auto& state = GetState();
            std::scoped_lock lock{state.mutex};
            FinishPending(state, when, false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
}
#endif

void StartFromEnvironment() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* const value = std::getenv("EDEN_NX2_SPIKES");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') {
            return;
        }
        const double ms = std::atof(value);
        if (ms > 1.0) {
            auto& state = GetState();
            std::scoped_lock lock{state.mutex};
            state.min_ms = ms;
        }
        SetEnabled(true);
    });
}

} // Anonymous namespace

bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    auto& state = GetState();
    std::unique_lock lock{state.mutex};
    if (enabled == g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (enabled) {
        state.started = Clock::now();
        state.has_last = false;
        state.frames.clear();
        state.turned_profiler_on = !Profile::Enabled();
        if (state.turned_profiler_on) {
            Profile::SetEnabled(true);
        }
        Profile::g_publish.store(true, std::memory_order_relaxed);
        g_counting.fetch_add(1, std::memory_order_relaxed);
        state.turned_ring_on = Common::Log::RingCapacity() == 0;
        if (state.turned_ring_on) {
            Common::Log::SetRingCapacity(RingLines);
        }
        g_enabled.store(true, std::memory_order_relaxed);
#ifdef __linux__
        GetSampler().thread = std::jthread(SamplerLoop);
#endif
        LOG_INFO(HW_GPU, "Frame spike recorder on: frames over {:.0f} ms and {:.1f}x the median",
                 state.min_ms, state.ratio);
        return;
    }
    g_enabled.store(false, std::memory_order_relaxed);
    g_counting.fetch_sub(1, std::memory_order_relaxed);
    FinishPending(state, std::nullopt, true);
    lock.unlock();
    // Joined without the state mutex: the sampler takes it.
    GetSampler().thread = {};
    lock.lock();
    Profile::g_publish.store(false, std::memory_order_relaxed);
    if (state.turned_profiler_on) {
        Profile::SetEnabled(false);
    }
    if (state.turned_ring_on) {
        Common::Log::SetRingCapacity(0);
    }
    LOG_INFO(HW_GPU, "Frame spike recorder off: {} spikes in {} presents", state.spikes,
             state.presents);
}

void Configure(double min_ms, double ratio) {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.min_ms = min_ms;
    state.ratio = ratio;
}

bool Counting() {
    return g_counting.load(std::memory_order_relaxed) > 0;
}

void Note(Event event, Clock::duration time, u64 amount) {
    if (!Counting()) {
        return;
    }
    const auto index = static_cast<size_t>(event);
    Shard& shard = ThisThreadShard();
    shard.count[index].fetch_add(1, std::memory_order_relaxed);
    if (time != Clock::duration{}) {
        shard.ns[index].fetch_add(static_cast<u64>(std::chrono::nanoseconds{time}.count()),
                                  std::memory_order_relaxed);
    }
    if (amount != 0) {
        shard.amount[index].fetch_add(amount, std::memory_order_relaxed);
    }
}

void SetPolled(Event event, u64 (*count)(), u64 (*ns)()) {
    const auto index = static_cast<size_t>(event);
    g_polled[index].count.store(count, std::memory_order_relaxed);
    g_polled[index].ns.store(ns, std::memory_order_relaxed);
}

void SetGuestThreadSource(GuestThreadSource source) {
    std::scoped_lock lock{g_guest_mutex};
    g_guest_source = source;
}

void OnPresent(Clock::time_point entered, Clock::time_point waited, Clock::time_point composited,
               Clock::time_point paced) {
    StartFromEnvironment();
    if (!Enabled()) {
        return;
    }
    auto gpu = TakeGpuSnapshot();
    const EventTotals events = ReadEvents();
    GuestSnapshot guest = TakeGuestSnapshot();
    const u64 log_sequence = Common::Log::RingNewestSequence();

    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    ++state.presents;
    if (state.has_last) {
        const auto frame = entered - state.last_entered;
        const double frame_ms = Ms(frame);
        const bool spike = state.frames.size() >= MinFramesForMedian &&
                           frame_ms >= state.min_ms && frame_ms >= state.ratio * state.median_ms;
        if (spike) {
            PendingSpike pending;
            Spike& s = pending.spike;
            s.number = ++state.spikes;
            s.present = state.presents;
            s.at_s = std::chrono::duration<double>(entered - state.started).count();
            s.frame_ms = frame_ms;
            s.median_ms = state.median_ms;
            s.game_ms = Ms(entered - state.last_paced);
            s.fence_ms = Ms(state.last_fence);
            s.composite_ms = Ms(state.last_composite);
            s.pace_ms = Ms(state.last_pace);
            s.gpu = GpuDifference(state.last_gpu, gpu, frame);
            s.events = EventDifference(state.last_events, events);
            s.guest = GuestDifference(state.last_guest, guest, GuestThreadsPerSpike);
            if (log_sequence > state.last_log_sequence) {
                const auto lines = Common::Log::ReadRing(state.last_log_sequence);
                const size_t first = lines.size() > LogLinesPerSpike ? lines.size() - LogLinesPerSpike : 0;
                for (size_t i = first; i < lines.size() && lines[i].sequence <= log_sequence; ++i) {
                    s.log.push_back(lines[i].text);
                }
            }
            // The frame is the previous present to this one; the host samples are taken every
            // 20 ms, so the sampler finishes the spike once one lands after it.
            pending.start = state.last_entered;
            pending.end = entered;
            state.pending.push_back(std::move(pending));
        }
        state.frames.push_back(frame_ms);
        if (state.frames.size() > MedianFrames) {
            state.frames.pop_front();
        }
        std::vector<double> sorted(state.frames.begin(), state.frames.end());
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        state.median_ms = sorted[sorted.size() / 2];
    }
#ifndef __linux__
    FinishPending(state, std::nullopt, true);
#else
    // A sampler that stopped answering must not hold spikes back for ever.
    for (const auto& pending : state.pending) {
        if (entered - pending.end > std::chrono::seconds{1}) {
            FinishPending(state, std::nullopt, true);
            break;
        }
    }
#endif
    state.has_last = true;
    state.last_entered = entered;
    state.last_paced = paced;
    state.last_fence = waited - entered;
    state.last_composite = composited - waited;
    state.last_pace = paced - composited;
    state.last_gpu = std::move(gpu);
    state.last_events = events;
    state.last_guest = std::move(guest);
    state.last_log_sequence = log_sequence;
}

Report Read(size_t limit) {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    Report report{
        .enabled = Enabled(),
        .min_ms = state.min_ms,
        .ratio = state.ratio,
        .presents = state.presents,
        .median_ms = state.median_ms,
        .spikes = state.spikes,
    };
    for (size_t i = 0; i < state.recent.size() && i < limit; ++i) {
        report.recent.push_back(state.recent[i]);
    }
    report.worst = state.worst;
    return report;
}

void Clear() {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.recent.clear();
    state.worst.clear();
    state.spikes = 0;
}

std::string Summary(const Spike& s) {
    std::string line = fmt::format(
        "Frame spike #{} at present {} ({:.1f} s): {:.1f} ms (median {:.1f}): game {:.1f}, GPU "
        "fence wait {:.1f}, composite {:.1f}, vblank pacing {:.1f}",
        s.number, s.present, s.at_s, s.frame_ms, s.median_ms, s.game_ms, s.fence_ms,
        s.composite_ms, s.pace_ms);
    for (const GpuThreadTimes& thread : s.gpu) {
        line += fmt::format(" | {}:", thread.thread);
        for (size_t i = 0; i < thread.zones.size() && i < 4; ++i) {
            line += fmt::format(" {} {:.1f}", thread.zones[i].zone, thread.zones[i].ms);
        }
    }
    if (s.host_sampled) {
        line += fmt::format(" | host CPU over {:.0f} ms:", s.host_span_ms);
        for (size_t i = 0; i < s.host.size() && i < 6; ++i) {
            line += fmt::format(" {} {:.1f}", s.host[i].name, s.host[i].cpu_ms);
            if (s.host[i].run_queue_ms >= 1.0) {
                line += fmt::format(" (waited for a CPU {:.1f})", s.host[i].run_queue_ms);
            }
        }
        line += fmt::format(" | page faults {} minor {} major, read {} KiB", s.minor_faults,
                            s.major_faults, s.read_kib);
    }
    if (!s.guest.empty()) {
        line += " | guest threads:";
        for (size_t i = 0; i < s.guest.size() && i < 4; ++i) {
            line += fmt::format(" {} ({}) {:.1f}", s.guest[i].name, s.guest[i].id, s.guest[i].cpu_ms);
        }
    }
    for (const EventTimes& event : s.events) {
        line += fmt::format(" | {} {} ({:.1f} ms", event.count, event.event, event.ms);
        if (event.unit != nullptr) {
            line += fmt::format(", {} {}", event.amount, event.unit);
        }
        line += ")";
    }
    return line;
}

struct Window::Impl {
    Clock::time_point started;
    EventTotals events;
    GuestSnapshot guest;
    bool finished{};
#ifdef __linux__
    HostSample host;
#endif
};

Window::Window() : impl{std::make_unique<Impl>()} {
    g_counting.fetch_add(1, std::memory_order_relaxed);
    impl->started = Clock::now();
    impl->events = ReadEvents();
    impl->guest = TakeGuestSnapshot();
#ifdef __linux__
    impl->host = TakeHostSample();
#endif
}

Window::~Window() {
    if (!impl->finished) {
        g_counting.fetch_sub(1, std::memory_order_relaxed);
    }
}

WindowReport Window::Finish(size_t max_threads) {
    WindowReport report;
    const EventTotals events = ReadEvents();
    const GuestSnapshot guest = TakeGuestSnapshot();
    report.seconds = std::chrono::duration<double>(Clock::now() - impl->started).count();
    report.events = EventDifference(impl->events, events);
    report.guest = GuestDifference(impl->guest, guest, max_threads);
#ifdef __linux__
    const HostSample host = TakeHostSample();
    std::unordered_map<int, std::string> names;
    report.host_sampled = true;
    report.host = HostDifference(impl->host, host, names, max_threads);
    report.minor_faults = host.minor_faults - impl->host.minor_faults;
    report.major_faults = host.major_faults - impl->host.major_faults;
    report.read_kib = (host.read_bytes - impl->host.read_bytes) / 1024;
#endif
    if (!impl->finished) {
        impl->finished = true;
        g_counting.fetch_sub(1, std::memory_order_relaxed);
    }
    return report;
}

} // namespace VideoCommon::Nx2Spikes
