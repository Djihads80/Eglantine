// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/nx2_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_quick_bind.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/semaphore_ledger.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        dma_pusher.rasterizer->FlushRegion(*device, sizeof(u32),
                                           VideoCommon::CacheType::BufferCache);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

void DrainBack() {
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::Nx2Profile::Zone::DrainAcquire);
}

using Ledger = Nx2SemaphoreLedger;

// A semaphore write the front just queued for the back: MME DMA reads of it wait for it.
void NotePendingSemaphore(DmaPusher& dma_pusher, GPUVAddr address) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
            split.NotePendingWrite(*device, 16);
        }
    }
}

// Records a 32-bit puller release the front just queued for the back (semaphore_ledger.h).
void NoteRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload) {
    NotePendingSemaphore(dma_pusher, address);
    if (!Ledger::Enabled()) {
        return;
    }
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        Ledger::Get().NoteRelease(*device, payload, VideoCommon::GpuSplit::Get().WritePosition(),
                                  dma_pusher.channel_state.bind_id);
    }
}

void ForgetRelease(DmaPusher& dma_pusher, GPUVAddr address) {
    NotePendingSemaphore(dma_pusher, address);
    if (!Ledger::Enabled()) {
        return;
    }
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        Ledger::Get().Forget(*device);
    }
}

// Whether an acquire is met without draining the split's back (semaphore_ledger.h): the last
// puller release queued for the word meets it and is still ahead of the back in the ring, or
// guest memory already meets it and no queued puller release moves the word away. The word is
// read without flushing the caches, which belong to the back: a value they hold but memory
// does not yet is newer, and sync words count up, so memory meeting the acquire is enough.
bool AcquireMetWithoutDrain(DmaPusher& dma_pusher, GPUVAddr address, u32 value,
                            Ledger::Compare compare) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!Ledger::Enabled() || !split.Active()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return false;
    }
    if (const Ledger::Entry* entry = Ledger::Get().Find(*device)) {
        const bool entry_met = Ledger::Met(compare, entry->value, value);
        const bool queued = entry->order > split.Consumed();
        if (queued) {
            // The word will hold this release's value when the back passes the acquire.
            return entry_met;
        }
        if (entry_met && Ledger::KeepsConsumed()) {
            // Run by the back, written at its fence: still ahead of the acquire's successors.
            return true;
        }
    }
    return Ledger::Met(compare, dma_pusher.memory_manager.Read<u32>(address), value);
}

Ledger::Compare CompareOf(u32 operation) {
    // Puller GpuSemaphoreOperation values (PendingAcquire::Matches).
    switch (operation) {
    case 0x1:
        return Ledger::Compare::Equal;
    case 0x4:
        return Ledger::Compare::Gequal;
    default:
        return Ledger::Compare::AndNonZero;
    }
}

void CountLedgerAcquire() {
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireLedger, 1);
}

// EDEN_NX2_ACQ_STATS: acquires that drained the back, by who last wrote their word.
struct AcquireStats {
    std::array<std::atomic<u64>, 4> drains{};
    std::array<std::atomic<u64>, 4> met{}; ///< Of the drains, the recorded release meets it
    std::array<std::atomic<u64>, 4> drain_ns{};
    std::array<std::atomic<u64>, 4> suspends{};
    std::array<std::atomic<u64>, 4> suspend_ns{};
    std::array<std::atomic<u64>, 4> resume_drains{}; ///< Drains polling a suspended acquire
    std::array<std::atomic<u64>, 4> resume_drain_ns{};
    std::array<std::atomic<u64>, 4> narrow{}; ///< Of the drains, met before the back ran out
    std::atomic<u64> window_start{};
};
AcquireStats g_acquire_stats;

// EDEN_NX2_ACQ_NARROW=1: an acquire that would drain the back waits only until the back has run
// far enough for AcquireMetWithoutDrain to hold (polled while the back works), not for all that
// was sent; when the back runs out of work first, it drains as before. With the split on Mario
// Kart World, ~80% of the front's drain time went to acquires on words compute QMDs release,
// and ~92% of those were met once drained.
bool AcquireNarrow() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQ_NARROW");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// EDEN_NX2_ACQ_VERIFY=1: every acquire the front lets pass without draining the back is checked
// on the back where it stands in the ring (the word flushed first): MISMATCH when the word does
// not meet it there, counted per way it passed and logged for the first ones.
enum class VerifyPath : u32 { Ledger, QmdSkip, Narrow, Resume, Count };
constexpr std::array<const char*, static_cast<size_t>(VerifyPath::Count)> verify_names{
    "ledger", "QMD skip", "narrow", "resume"};
struct AcquireVerify {
    std::array<std::atomic<u64>, static_cast<size_t>(VerifyPath::Count)> checked{};
    std::array<std::atomic<u64>, static_cast<size_t>(VerifyPath::Count)> mismatches{};
};
AcquireVerify g_acquire_verify;

bool AcquireVerifyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQ_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// 0: a puller release of this channel, 1: of another channel, 2: a compute QMD release, 3: none
// the front saw (the CPU, 3D reports). `met`: the recorded release meets the acquire.
int AcquireClass(DmaPusher& dma_pusher, GPUVAddr address, u32 value, Ledger::Compare compare,
                 bool& met) {
    met = false;
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return 3;
    }
    if (const Ledger::Entry* entry = Ledger::Get().Find(*device)) {
        met = Ledger::Met(compare, entry->value, value);
        return entry->channel == dma_pusher.channel_state.bind_id ? 0 : 1;
    }
    return Nx2QmdReleaseTargets::Contains(*device) ? 2 : 3;
}

u64 SteadyNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void ReportAcquireStats() {
    AcquireStats& stats = g_acquire_stats;
    const u64 now = SteadyNowNs();
    u64 start = stats.window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 5'000'000'000ULL ||
        !stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        return;
    }
    static constexpr std::array<const char*, 4> names{"same channel", "other channel",
                                                      "compute QMD", "CPU or report"};
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const double seconds = static_cast<double>(now - start) / 1e9;
    std::string line;
    for (size_t i = 0; i < names.size(); ++i) {
        const u64 drains = take(stats.drains[i]);
        const u64 met = take(stats.met[i]);
        const u64 drain_ns = take(stats.drain_ns[i]);
        const u64 suspends = take(stats.suspends[i]);
        const u64 suspend_ns = take(stats.suspend_ns[i]);
        const u64 resume_drains = take(stats.resume_drains[i]);
        const u64 resume_drain_ns = take(stats.resume_drain_ns[i]);
        const u64 narrow = take(stats.narrow[i]);
        line += fmt::format(" | {}: {:.0f} drains ({:.0f} met by the release, {:.0f} narrow) {:.1f} "
                            "ms, {:.0f} suspended {:.1f} ms, {:.0f} resume drains {:.1f} ms",
                            names[i], static_cast<double>(drains) / seconds,
                            static_cast<double>(met) / seconds,
                            static_cast<double>(narrow) / seconds,
                            static_cast<double>(drain_ns) / 1e6 / seconds,
                            static_cast<double>(suspends) / seconds,
                            static_cast<double>(suspend_ns) / 1e6 / seconds,
                            static_cast<double>(resume_drains) / seconds,
                            static_cast<double>(resume_drain_ns) / 1e6 / seconds);
    }
    LOG_INFO(HW_GPU, "Split acquires that drained, per second over {:.1f} s{}", seconds, line);
    if (AcquireVerifyEnabled()) {
        std::string verify;
        for (size_t i = 0; i < verify_names.size(); ++i) {
            verify += fmt::format(" | {}: {} checked, {} MISMATCH", verify_names[i],
                                  g_acquire_verify.checked[i].load(std::memory_order_relaxed),
                                  g_acquire_verify.mismatches[i].load(std::memory_order_relaxed));
        }
        LOG_INFO(HW_GPU, "Split acquire verify, since boot{}", verify);
    }
}

// The acquire's word where the back reaches it in the ring (EDEN_NX2_ACQ_VERIFY).
void VerifyOnBack(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire, VerifyPath path) {
    if (!AcquireVerifyEnabled() || !VideoCommon::GpuSplit::Get().Active()) {
        return;
    }
    VideoCommon::GpuSplit::Get().Defer([dma = &dma_pusher, address = acquire.address,
                                        value = acquire.value, operation = acquire.operation,
                                        path] {
        const Puller::PendingAcquire check{
            .address = address,
            .value = value,
            .operation = operation,
            .since = {},
            .last_fences = {},
            .reported = false,
        };
        const u32 word = ReadSemaphoreWord(*dma, address);
        const auto index = static_cast<size_t>(path);
        g_acquire_verify.checked[index].fetch_add(1, std::memory_order_relaxed);
        if (!check.Matches(word)) {
            const u64 count =
                g_acquire_verify.mismatches[index].fetch_add(1, std::memory_order_relaxed);
            if (count < 16) {
                LOG_WARNING(HW_GPU,
                            "Split acquire MISMATCH ({}): {:#x} op {} value {:#x}, the word is "
                            "{:#x} where the back reaches the acquire",
                            verify_names[index], address, operation, value, word);
            }
        }
    });
}

struct NarrowContext {
    DmaPusher* dma_pusher;
    const Puller::PendingAcquire* acquire;
};

bool NarrowMet(void* context) {
    const auto& narrow = *static_cast<const NarrowContext*>(context);
    return AcquireMetWithoutDrain(*narrow.dma_pusher, narrow.acquire->address,
                                  narrow.acquire->value, CompareOf(narrow.acquire->operation));
}

// EDEN_NX2_QMD_ACQUIRE_SKIP=1: an acquire on a word compute QMDs release does not drain the back
// (semaphore_ledger.h, Nx2QmdReleaseTargets).
bool QmdAcquireSkip(DmaPusher& dma_pusher, GPUVAddr address) {
    if (!Nx2QmdReleaseTargets::Enabled() || !VideoCommon::GpuSplit::Get().Active()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    return device && Nx2QmdReleaseTargets::Contains(*device);
}

} // Anonymous namespace

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    if (AcquireMetWithoutDrain(dma_pusher, pending_acquire->address, pending_acquire->value,
                               CompareOf(pending_acquire->operation))) {
        CountLedgerAcquire();
        VerifyOnBack(dma_pusher, *pending_acquire, VerifyPath::Resume);
        return true;
    }
    // A release latched the wait (Nx2SemaphoreWaiters): the back has run it, so resume without
    // draining (from sylvieistaken/selevet-eglantine d8c95fef47). EDEN_NX2_ACQUIRE_PEEK=0: drain.
    static const bool peek_latched = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_PEEK");
        return value == nullptr || value[0] != '0';
    }();
    if (peek_latched && VideoCommon::GpuSplit::Get().Active() &&
        Nx2SemaphoreWaiters::Get().Latched(this)) {
        return true;
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    const int stats_class = pending_acquire->stats_class;
    const u64 drain_start = stats_class >= 0 ? SteadyNowNs() : 0;
    DrainBack();
    if (stats_class >= 0) {
        g_acquire_stats.resume_drains[stats_class].fetch_add(1, std::memory_order_relaxed);
        g_acquire_stats.resume_drain_ns[stats_class].fetch_add(SteadyNowNs() - drain_start,
                                                               std::memory_order_relaxed);
    }
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (static std::atomic<u32> rescued{};
            !pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, current);
        }
        return true;
    }
    if (pending_acquire->Matches(current)) {
        return true;
    }
    // With delayed fence operations (Balanced or Accurate fences, accurate GPU), a release
    // queued after the acquire released the fences waits in the caches for the next fence. With
    // every channel waiting none comes: release them again now and then while waiting (the back
    // was drained above). EDEN_NX2_ACQUIRE_REFENCE=0: only the read, as before.
    static const bool refence = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_REFENCE");
        return value == nullptr || value[0] != '0';
    }();
    const bool delayed = Settings::IsGPUFenceBehaviorDefault()
                             ? Settings::IsGPULevelHigh()
                             : Settings::IsGPUFenceBehaviorBalanced() ||
                                   Settings::IsGPUFenceBehaviorAccurate();
    if (const auto now = std::chrono::steady_clock::now();
        refence && delayed && now - pending_acquire->last_fences >= std::chrono::milliseconds{1}) {
        pending_acquire->last_fences = now;
        dma_pusher.rasterizer->ReleaseFences();
        if (Nx2SemaphoreWaiters::Get().Latched(this) ||
            pending_acquire->Matches(ReadSemaphoreWord(dma_pusher, pending_acquire->address))) {
            return true;
        }
    }
    if (!pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since > std::chrono::seconds{2}) {
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for 2 s; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current, device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
        LOG_WARNING(HW_GPU, "  channel {} waits in {}", dma_pusher.channel_state.bind_id,
                    dma_pusher.DescribeCurrentEntry());
        // The QMDs that release the word (queue syncs): whether one waits for dependencies or
        // already ran with an older payload. Their table belongs to the thread running engine
        // methods, this one without the split.
        if (!VideoCommon::GpuSplit::Get().Active()) {
            u32 listed = 0;
            NativeQmdScheduler::ForEachReleaseTo(
                pending_acquire->address, [&](u64 qmd, bool loaded, bool scheduled, bool completed,
                                              s32 dependencies, s32 deferred, u32 payload) {
                    if (listed++ < 8) {
                        LOG_WARNING(HW_GPU,
                                    "  QMD {:#x} releases it: payload {:#x}, loaded {} scheduled "
                                    "{} completed {}, {} dependencies, {} deferred",
                                    qmd, payload, loaded, scheduled, completed, dependencies,
                                    deferred);
                    }
                });
            if (listed == 0) {
                LOG_WARNING(HW_GPU, "  no QMD seen releases it");
            }
        }
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    if (pending_acquire) {
        // What the CPU wrote while the channel waited (texture pools among it).
        Nx2BumpPoolGeneration();
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those made the count odd ("suspended") at random, so submit-time snapshots were
    // thrown away for about half of all entries (CommandList::snapshot_suspensions).
    if (pending_acquire) {
        if (const int stats_class = pending_acquire->stats_class; stats_class >= 0) {
            g_acquire_stats.suspends[stats_class].fetch_add(1, std::memory_order_relaxed);
            g_acquire_stats.suspend_ns[stats_class].fetch_add(
                SteadyNowNs() - pending_acquire->stats_since, std::memory_order_relaxed);
        }
        pending_acquire.reset();
        dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
        Nx2SemaphoreWaiters::Get().Remove(this);
    }
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        const GPUVAddr mirror = g_nx2_ampere ? Nx2SemaphoreMirror(sequence_address) : 0;
        if (mirror != 0) {
            LOG_DEBUG(HW_GPU, "Switch 2 semaphore {:#x}={:#x} mirrored to {:#x}", sequence_address, payload, mirror);
        }
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, mirror, properties, payload] {
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
            if (mirror != 0) {
                rasterizer->Query(mirror, VideoCommon::QueryType::Payload, properties, payload, 0);
            }
        });
        NoteRelease(dma_pusher, sequence_address, payload);
        if (mirror != 0) {
            NoteRelease(dma_pusher, mirror, payload);
        }
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        Nx2BumpPoolGeneration();
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .last_fences = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireChecks, 1);
        if (AcquireMetWithoutDrain(dma_pusher, acquire.address, acquire.value,
                                   CompareOf(acquire.operation))) {
            CountLedgerAcquire();
            VerifyOnBack(dma_pusher, acquire, VerifyPath::Ledger);
            return;
        }
        if (QmdAcquireSkip(dma_pusher, acquire.address)) {
            CountLedgerAcquire();
            VerifyOnBack(dma_pusher, acquire, VerifyPath::QmdSkip);
            return;
        }
        const bool stats = Nx2AcqStats() && VideoCommon::GpuSplit::Get().Active();
        bool stats_met = false;
        const int stats_class =
            stats ? AcquireClass(dma_pusher, acquire.address, acquire.value,
                                 CompareOf(acquire.operation), stats_met)
                  : -1;
        const u64 drain_start = stats ? SteadyNowNs() : 0;
        bool narrow_met = false;
        if (AcquireNarrow() && VideoCommon::GpuSplit::Get().Active()) {
            NarrowContext context{&dma_pusher, &acquire};
            narrow_met = VideoCommon::GpuSplit::Get().WaitUntil(
                &NarrowMet, &context, VideoCommon::Nx2Profile::Zone::DrainAcquire);
        }
        if (!narrow_met) {
            DrainBack();
        }
        if (stats) {
            g_acquire_stats.narrow[stats_class].fetch_add(narrow_met ? 1 : 0,
                                                          std::memory_order_relaxed);
            g_acquire_stats.drains[stats_class].fetch_add(1, std::memory_order_relaxed);
            g_acquire_stats.met[stats_class].fetch_add(stats_met ? 1 : 0,
                                                       std::memory_order_relaxed);
            g_acquire_stats.drain_ns[stats_class].fetch_add(SteadyNowNs() - drain_start,
                                                            std::memory_order_relaxed);
            ReportAcquireStats();
        }
        if (narrow_met) {
            CountLedgerAcquire();
            VerifyOnBack(dma_pusher, acquire, VerifyPath::Narrow);
            return;
        }
        if (acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        dma_pusher.rasterizer->ReleaseFences();
        if (!waiters.Latched(this) && !acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            pending_acquire = acquire;
            pending_acquire->stats_class = stats_class;
            pending_acquire->stats_since = stats ? SteadyNowNs() : 0;
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
        } else {
            waiters.Remove(this);
        }
    } else {
        DrainBack();
        do {
            const u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
    NoteRelease(dma_pusher, sequence_address, payload);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    Nx2BumpPoolGeneration();
    const auto value = regs.semaphore_acquire;
    if (AcquireMetWithoutDrain(dma_pusher, regs.semaphore_address.SemaphoreAddress(), value,
                               Ledger::Compare::Equal)) {
        CountLedgerAcquire();
        return;
    }
    DrainBack();
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        dma_pusher.rasterizer->ReleaseFences();
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    // PAYLOAD_SIZE (bit 24): a 64-bit payload (SEM_PAYLOAD_HI, 0x1a). Only the low word is
    // written and compared here; the ledger stays out of such words.
    const bool payload_64 = (argument & (1U << 24)) != 0;
    if (operation == 1) { // RELEASE
        OnBack([rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        if (payload_64) {
            ForgetRelease(dma_pusher, address);
        } else {
            NoteRelease(dma_pusher, address, payload);
        }
        return;
    }
    if (operation == 6) { // REDUCTION: the word's value depends on what it held
        ForgetRelease(dma_pusher, address);
    }
    if (operation != 6) {
        Nx2BumpPoolGeneration(); // an acquire
    }
    if (!payload_64 && operation <= 5 && operation != 1) {
        const Ledger::Compare compare = operation == 0   ? Ledger::Compare::Equal
                                        : operation == 2 ? Ledger::Compare::Gequal
                                        : operation == 3 ? Ledger::Compare::CircularGequal
                                        : operation == 4 ? Ledger::Compare::AndNonZero
                                                         : Ledger::Compare::Nor;
        if (AcquireMetWithoutDrain(dma_pusher, address, payload, compare)) {
            CountLedgerAcquire();
            return;
        }
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    // The check only logs: with the split the back releases the fences in order instead of the
    // front draining it (from sylvieistaken/selevet-eglantine d8c95fef47).
    // EDEN_NX2_SEMEXEC_DRAIN=1 drains and checks as before.
    static const bool semexec_drain = [] {
        const char* value = std::getenv("EDEN_NX2_SEMEXEC_DRAIN");
        return value != nullptr && value[0] == '1';
    }();
    if (!semexec_drain && VideoCommon::GpuSplit::Get().Active()) {
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        return;
    }
    DrainBack();
    dma_pusher.rasterizer->ReleaseFences();
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    const bool ready = operation == 0   ? word == payload
                       : operation == 2 ? word >= payload
                       : operation == 3 ? static_cast<s32>(word - payload) >= 0
                       : operation == 4 ? (word & payload) != 0
                       : operation == 5 ? (word & payload) == 0
                                        : true;
    if (!ready) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation C,D");
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
