// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
extern std::atomic<u64> g_nx2_present_count;
}
namespace VideoCommon::CommandWriterTrace {
constexpr u64 PageSize = 4096;
constexpr u64 FirstPresent = 3300, LastPresent = 3500;
constexpr size_t MaxWatches = 128, MaxEvents = 32768;
// Keep broad small-entry coverage bounded, with room for the 13-word channel-2 occurrence
// whose targets changed in immutable26. This does not admit every descriptor up to16 words.
constexpr size_t ReservedTargetedWatches = 16;
constexpr size_t MaxOrdinaryWatches = MaxWatches - ReservedTargetedWatches;
constexpr size_t MaxSnapshotWords = 8;
constexpr bool IsTargetedDescriptor(s32 channel, u64 words) {
    return channel == 2 && words == 13;
}
constexpr bool WatchesDescriptor(s32 channel, u64 words) {
    return IsTargetedDescriptor(channel, words) ||
           ((channel == 1 || channel == 2) && words > 0 && words <= MaxSnapshotWords);
}
enum class Kind : u32 {
    Snapshot,
    CpuCachedAccess,
    GpuDirtyMark,
    GpuReadCopy,
    GpuScalarWrite,
    GpuBlockWrite,
    DeviceReadCopy,
    DeviceScalarWrite,
    DeviceBlockWrite,
    DeviceUnsafeWrite,
    GpuMap,
    GpuSparseMap,
    GpuUnmap,
    DeviceMap,
    DeviceUnmap,
    QueryReportWrite,
    QueryImmediateWrite,
    QueryPartialWrite,
    CpuCompletedWrite
};
enum class Phase : u32 { Before, After };
enum class Domain : u32 { Device, Gpu };
enum class Extent : u32 { ExactBytes, PageCoverage };
struct Range {
    u64 address{}, size{};
};
inline std::optional<Range> Intersect(Range a, Range b) {
    constexpr u64 max = std::numeric_limits<u64>::max();
    if (!a.size || !b.size || a.size > max - a.address || b.size > max - b.address)
        return {};
    const u64 first = std::max(a.address, b.address);
    const u64 end = std::min(a.address + a.size, b.address + b.size);
    if (first >= end)
        return {};
    return Range{first, end - first};
}
struct Watch {
    u64 id{}, gpu_asid{}, gpu_page{}, device_page{}, owner{};
    bool targeted{};
};
class Registry {
  public:
    bool Add(u64 gpu_asid, u64 gpu_page, u64 device_page, u64 owner, bool targeted = false) {
        if (((gpu_page | device_page) & (PageSize - 1)) != 0 ||
            gpu_page > std::numeric_limits<u64>::max() - PageSize ||
            device_page > std::numeric_limits<u64>::max() - PageSize) {
            return false;
        }
        for (auto& w : watches) {
            if (w.gpu_asid == gpu_asid && w.gpu_page == gpu_page && w.device_page == device_page &&
                w.owner == owner) {
                if (targeted && !w.targeted) {
                    w.targeted = true;
                    --ordinary_count;
                }
                return true;
            }
        }
        if (watches.size() == MaxWatches || (!targeted && ordinary_count == MaxOrdinaryWatches)) {
            ++(targeted ? dropped_targeted : dropped_ordinary);
            return false;
        }
        watches.push_back({watches.size() + 1, gpu_asid, gpu_page, device_page, owner, targeted});
        if (!targeted) {
            ++ordinary_count;
        }
        return true;
    }
    template <typename Func> void Device(u64 owner, Range range, Func &&func) const {
        for (const auto &w : watches) {
            if (owner && owner != w.owner)
                continue;
            if (auto overlap = Intersect(range, {w.device_page, PageSize}))
                func(w, *overlap);
        }
    }
    template <typename Func> void Gpu(u64 asid, Range range, Func &&func) const {
        for (const auto &w : watches) {
            if (w.gpu_asid != asid)
                continue;
            if (auto overlap = Intersect(range, {w.gpu_page, PageSize}))
                func(w, *overlap);
        }
    }
    const std::vector<Watch> &Watches() const { return watches; }
    size_t OrdinaryCount() const { return ordinary_count; }
    size_t TargetedCount() const { return watches.size() - ordinary_count; }
    u64 DroppedOrdinary() const { return dropped_ordinary; }
    u64 DroppedTargeted() const { return dropped_targeted; }
    u64 Dropped() const { return dropped_ordinary + dropped_targeted; }

  private:
    std::vector<Watch> watches;
    size_t ordinary_count{};
    u64 dropped_ordinary{}, dropped_targeted{};
};
struct CpuContext {
    u64 pid{}, core{}, block_pc{};
};
inline thread_local CpuContext cpu_context{};
inline bool Enabled() {
    static const bool enabled = [] {
        const char *gate = std::getenv("EDEN_NX2_COMMAND_WRITER_TRACE");
        const char *trace = std::getenv("EDEN_NX2_COMMAND_TRACE");
        const char *path = std::getenv("EDEN_NX2_COMMAND_TRACE_FILE");
        return gate && std::strcmp(gate, "1") == 0 && trace && trace[0] != '\0' &&
               trace[0] != '0' && path && path[0] != '\0';
    }();
    return enabled;
}
inline bool Active() {
    if (!Enabled())
        return false;
    const u64 present = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    return present >= FirstPresent && present <= LastPresent;
}
// Only existing Dynarmic slow write callbacks install a PC; direct fastmem stores remain unseen.
class CpuScope {
  public:
    template <typename ReadContext> explicit CpuScope(ReadContext &&read_context) {
        if (Active()) {
            previous = cpu_context;
            cpu_context = read_context();
        }
    }
    ~CpuScope() {
        if (previous)
            cpu_context = *previous;
    }
    CpuScope(const CpuScope &) = delete;
    CpuScope &operator=(const CpuScope &) = delete;

  private:
    std::optional<CpuContext> previous;
};
struct Event {
    u64 ns{}, present{}, watch_id{};
    Kind kind{};
    Phase phase{};
    Domain domain{};
    Extent extent{};
    Range range{}, overlap{};
    // aux0/aux1: caller-specific metadata, documented by kind; never dereferenced.
    u64 aux0{}, aux1{}, submission{}, entry_index{}, descriptor{}, suspension_epoch{};
    u64 entry_words{};
    s32 snapshot_channel{-1};
    CpuContext cpu{};
    std::array<u32, 3> words{};
};
template <size_t N> class Ring {
    static_assert(N > 0);

  public:
    void Append(Event e) { records[total++ % N] = e; }
    std::vector<Event> Snapshot() const {
        std::vector<Event> result;
        for (u64 i = total - std::min<u64>(total, N); i < total; ++i)
            result.push_back(records[i % N]);
        return result;
    }
    u64 Total() const { return total; }

  private:
    std::array<Event, N> records{};
    u64 total{};
};
struct State {
    std::mutex mutex;
    Registry registry;
    Ring<MaxEvents> events;
    bool dumped{};
    std::atomic<u64> low{std::numeric_limits<u64>::max()}, high{};
};
inline State &GetState() {
    static State state;
    return state;
}
inline u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void WatchPage(u64 asid, u64 gpu_page, u64 device_page, u64 owner, bool targeted = false) {
    if (!Active())
        return;
    auto &s = GetState();
    std::scoped_lock lk{s.mutex};
    if (s.dumped || !s.registry.Add(asid, gpu_page, device_page, owner, targeted))
        return;
    s.low.store(std::min(s.low.load(std::memory_order_relaxed), device_page),
                std::memory_order_release);
    s.high.store(std::max(s.high.load(std::memory_order_relaxed), device_page + PageSize),
                 std::memory_order_release);
}
inline void RecordDevice(Kind kind, Phase phase, u64 owner, u64 address, u64 size, u64 aux0 = 0,
                         u64 aux1 = 0, Extent extent = Extent::ExactBytes) {
    if (!Active())
        return;
    auto &s = GetState();
    const u64 lo = s.low.load(std::memory_order_acquire),
              hi = s.high.load(std::memory_order_acquire);
    if (hi <= lo || !Intersect({address, size}, {lo, hi - lo}))
        return;
    std::scoped_lock lk{s.mutex};
    if (s.dumped)
        return;
    u64 ns{};
    s.registry.Device(owner, {address, size}, [&](const Watch &w, Range overlap) {
        if (!ns)
            ns = Now();
        s.events.Append({.ns = ns,
                         .present = Core::g_nx2_present_count.load(std::memory_order_relaxed),
                         .watch_id = w.id,
                         .kind = kind,
                         .phase = phase,
                         .domain = Domain::Device,
                         .extent = extent,
                         .range = {address, size},
                         .overlap = overlap,
                         .aux0 = aux0,
                         .aux1 = aux1,
                         .cpu = cpu_context});
    });
}
inline void RecordGpu(Kind kind, Phase phase, u64 asid, u64 address, u64 size, u64 aux0 = 0,
                      u64 aux1 = 0, Extent extent = Extent::ExactBytes) {
    if (!Active())
        return;
    auto &s = GetState();
    std::scoped_lock lk{s.mutex};
    if (s.dumped)
        return;
    u64 ns{};
    std::vector<Watch> new_aliases;
    s.registry.Gpu(asid, {address, size}, [&](const Watch &w, Range overlap) {
        if (!ns)
            ns = Now();
        s.events.Append({.ns = ns,
                         .present = Core::g_nx2_present_count.load(std::memory_order_relaxed),
                         .watch_id = w.id,
                         .kind = kind,
                         .phase = phase,
                         .domain = Domain::Gpu,
                         .extent = extent,
                         .range = {address, size},
                         .overlap = overlap,
                         .aux0 = aux0,
                         .aux1 = aux1,
                         .cpu = cpu_context});
        // Retain old aliases too: earlier queued occurrences can still refer to them.
        // Only a complete aligned mapped page establishes a new device alias here.
        if (kind == Kind::GpuMap && phase == Phase::After && w.gpu_page >= address &&
            size >= PageSize && w.gpu_page - address <= size - PageSize &&
            aux0 <= std::numeric_limits<u64>::max() - (w.gpu_page - address)) {
            new_aliases.push_back({0, asid, w.gpu_page, aux0 + w.gpu_page - address, w.owner,
                                   w.targeted});
        }
    });
    for (const auto &w : new_aliases) {
        if (s.registry.Add(w.gpu_asid, w.gpu_page, w.device_page, w.owner, w.targeted)) {
            s.low.store(std::min(s.low.load(std::memory_order_relaxed), w.device_page),
                        std::memory_order_release);
            s.high.store(std::max(s.high.load(std::memory_order_relaxed), w.device_page + PageSize),
                         std::memory_order_release);
        }
    }
}
template <typename Word>
void Snapshot(s32 channel, u64 asid, u64 address, u64 submission, u64 index, u64 descriptor,
              u64 epoch, bool refreshed, std::span<const Word> words) {
    if (!Active() || !WatchesDescriptor(channel, words.size()))
        return;
    auto &s = GetState();
    std::scoped_lock lk{s.mutex};
    if (s.dumped)
        return;
    const auto ns = Now();
    s.registry.Gpu(asid, {address, words.size() * sizeof(u32)}, [&](const Watch &w, Range overlap) {
        Event e{.ns = ns,
                .present = Core::g_nx2_present_count.load(std::memory_order_relaxed),
                .watch_id = w.id,
                .kind = Kind::Snapshot,
                .phase = Phase::After,
                .domain = Domain::Gpu,
                .range = {address, words.size() * sizeof(u32)},
                .overlap = overlap,
                .aux0 = refreshed ? 1U : 0U,
                .submission = submission,
                .entry_index = index,
                .descriptor = descriptor,
                .suspension_epoch = epoch,
                .entry_words = words.size(),
                .snapshot_channel = channel};
        for (size_t i = 0; i < std::min(words.size(), e.words.size()); ++i)
            e.words[i] = words[i].argument;
        s.events.Append(e);
    });
}
inline void Dump(const char *path, const char *reason) {
    if (!Enabled())
        return;
    auto &s = GetState();
    std::vector<Watch> watches;
    std::vector<Event> events;
    u64 total{}, dropped{}, dropped_ordinary{}, dropped_targeted{};
    size_t ordinary_count{}, targeted_count{};
    {
        std::scoped_lock lk{s.mutex};
        if (s.dumped)
            return;
        s.dumped = true;
        watches = s.registry.Watches();
        events = s.events.Snapshot();
        total = s.events.Total();
        dropped = s.registry.Dropped();
        dropped_ordinary = s.registry.DroppedOrdinary();
        dropped_targeted = s.registry.DroppedTargeted();
        ordinary_count = s.registry.OrdinaryCount();
        targeted_count = s.registry.TargetedCount();
    }
    std::ofstream out(std::string{path} + ".writers.csv", std::ios::trunc);
    out << "# reason=" << reason << " total=" << total << " retained=" << events.size()
        << " watches=" << watches.size() << " dropped_watches=" << dropped
        << " ordinary_watches=" << ordinary_count << " targeted_watches=" << targeted_count
        << " dropped_ordinary=" << dropped_ordinary << " dropped_targeted=" << dropped_targeted
        << " reserved_targeted=" << ReservedTargetedWatches << '\n';
    out << "kind,phase,domain,extent,ns,present,watch_id,gpu_asid,gpu_page,device_page,owner,"
           "address,size,overlap_address,overlap_size,aux0,aux1,submission,entry_index,descriptor,"
           "suspension_epoch,pid,core,pc_kind,block_pc,snapshot_channel,entry_words,w0,w1,w2,watch_targeted_at_dump\n";
    for (const auto &e : events) {
        const auto &w = watches.at(e.watch_id - 1);
        out << static_cast<u32>(e.kind) << ',' << static_cast<u32>(e.phase) << ','
            << static_cast<u32>(e.domain) << ',' << static_cast<u32>(e.extent) << ',' << e.ns << ','
            << e.present << ',' << w.id << ',' << w.gpu_asid << ',' << w.gpu_page << ','
            << w.device_page << ',' << w.owner << ',' << e.range.address << ',' << e.range.size
            << ',' << e.overlap.address << ',' << e.overlap.size << ',' << e.aux0 << ',' << e.aux1
            << ',' << e.submission << ',' << e.entry_index << ',' << e.descriptor << ','
            << e.suspension_epoch << ',' << e.cpu.pid << ',' << e.cpu.core << ','
            << (e.cpu.block_pc ? 1 : 0) << ',' << e.cpu.block_pc << ','
            << e.snapshot_channel << ',' << e.entry_words;
        for (auto word : e.words)
            out << ',' << word;
        out << ',' << (w.targeted ? 1 : 0) << '\n';
    }
}
} // namespace VideoCommon::CommandWriterTrace
