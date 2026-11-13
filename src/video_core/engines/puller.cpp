// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

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
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        // The flush downloads through the Vulkan scheduler, which the split's back owns.
        VideoCommon::GpuSplit::Get().Run(
            [rasterizer = dma_pusher.rasterizer, device = *device] {
                rasterizer->FlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::DrainSite::SemaphoreFlush);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func,
            VideoCommon::GpuSplit::Effect effect = VideoCommon::GpuSplit::Effect::Unknown) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func, effect);
    } else {
        func();
    }
}

// A semaphore release: it writes the payload (and timestamp) at `address` and nothing else.
template <typename Func>
void OnBackRelease(DmaPusher& dma_pusher, GPUVAddr address, const Func& func) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active()) {
        func();
        return;
    }
    split.Defer(func, VideoCommon::GpuSplit::Effect::None);
    split.NoteWrite(dma_pusher.memory_manager, address, 16);
    // Fence releases go through the query cache and can land later than the back runs them:
    // early reads of these bytes wait for the result (lichii #179, 166c21d96c).
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        split.NoteAsyncWrite(*device, 16);
    }
}

void DrainBack(VideoCommon::DrainSite site = VideoCommon::DrainSite::Acquire) {
    VideoCommon::GpuSplit::Get().Drain(site);
}

// The fence manager records into the scheduler and downloads the CPU's writes over GPU data:
// the back's state, which a drained back still uses from its idle hook (it submits).
void ReleaseFences(DmaPusher& dma_pusher) {
    VideoCommon::GpuSplit::Get().Run(
        [rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
        VideoCommon::DrainSite::FenceRelease);
}

// A release was just sent to the back: tell the front what it writes (CheckWithoutDrain).
void NoteRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload, bool four_words) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active()) {
        return;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return;
    }
    split.NoteSemaphoreWrite(*device, payload, true);
    if (four_words) {
        // The payload as 64 bits (a timestamp follows).
        split.NoteSemaphoreWrite(*device + 4, 0, true);
    }
}

// Whether the back writes a payload release to memory when it runs it (CounterReport): below
// High GPU accuracy, with fences that run their operations at once (FenceManager::SignalFence).
bool ReleasesLandAtOnce() {
    const bool delay_fence =
        Settings::IsGPUFenceBehaviorDefault()
            ? Settings::IsGPULevelHigh()
            : Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
    return !delay_fence && !Settings::IsGPULevelHigh();
}

enum class AcquireCheck {
    Met,         ///< By the word in memory
    MetByQueued, ///< By a release sent to the word that the back has not run yet
    NotMet,      ///< Nothing sent to the word can meet it yet
    Drain,       ///< Drain the split and read the word as before
};

// Why an acquire trigger drained the split (Nx2Profile counters AcquireDrain*).
enum class AcquireDrainReason : u32 {
    NotGequal,     ///< Not a greater-or-equal acquire
    Setup,         ///< No split front macros, or releases don't land when the back runs them
    GpuWritten,    ///< Unmapped, or the buffer cache may hold the word newer than memory
    QueuedUnknown, ///< The last queued release to the word has a value the front can't tell
    QueuedLower,   ///< The last queued release to the word doesn't meet the value
    NotMet,        ///< Nothing queued to the word, and memory doesn't meet the value
};

// EDEN_NX2_ACQ_STATS=1: per semaphore word, the acquire trigger drains and the suspensions
// after them (how long, met by a GPU release or a word read), logged every 4000 suspensions.
struct AcquireStats {
    struct Word {
        u32 operation{};
        u32 channel{};
        u64 drains{};
        u64 drain_ns{};
        u64 suspensions{};
        u64 suspended_ns{};
        u64 by_release{}; ///< Resumed by a latched GPU release
        u64 queued{};     ///< A release to the word was queued for the back at the suspension
        u32 value{};
        u32 word{};
    };
    static bool On() {
        static const bool on = std::getenv("EDEN_NX2_ACQ_STATS") != nullptr;
        return on;
    }
    static AcquireStats& Get() {
        static AcquireStats stats;
        return stats;
    }
    std::mutex mutex;
    std::unordered_map<u64, Word> words; // by GPU address
    u64 total_suspensions{};

    void NoteDrain(GPUVAddr address, u32 operation, u32 channel, u32 value, u32 word, u64 ns) {
        std::scoped_lock lock{mutex};
        Word& w = words[address];
        w.operation = operation;
        w.channel = channel;
        w.value = value;
        w.word = word;
        ++w.drains;
        w.drain_ns += ns;
    }
    void NoteSuspended(GPUVAddr address, bool queued) {
        std::scoped_lock lock{mutex};
        words[address].queued += queued ? 1 : 0;
    }
    void NoteResumed(GPUVAddr address, u64 ns, bool by_release) {
        std::scoped_lock lock{mutex};
        Word& w = words[address];
        ++w.suspensions;
        w.suspended_ns += ns;
        w.by_release += by_release ? 1 : 0;
        if (++total_suspensions % 4000 != 0) {
            return;
        }
        std::vector<std::pair<u64, const Word*>> sorted;
        for (const auto& [addr, word] : words) {
            sorted.emplace_back(addr, &word);
        }
        std::ranges::sort(sorted, [](const auto& a, const auto& b) {
            return a.second->drain_ns + a.second->suspended_ns >
                   b.second->drain_ns + b.second->suspended_ns;
        });
        std::string line;
        for (size_t i = 0; i < (std::min<size_t>)(sorted.size(), 8); ++i) {
            const auto& [addr, word] = sorted[i];
            line += fmt::format(
                " | {:#x} op {} ch {}: {} drains {:.1f} ms, {} suspended {:.1f} ms ({} by GPU release, "
                "{} with one queued), last value {:#x} word {:#x}",
                addr, word->operation, word->channel, word->drains, word->drain_ns / 1e6,
                word->suspensions, word->suspended_ns / 1e6, word->by_release, word->queued,
                word->value, word->word);
        }
        LOG_INFO(HW_GPU, "Acquire stats after {} suspensions:{}", total_suspensions, line);
    }
};

// Checks an acquire without draining the split. The words Switch 2 syncs on only advance (NVN2
// even releases QMD completions as maxima), so a value that meets one now still does once the
// back ran the queued work. Only the last release the front sent to the word decides instead,
// when the back may not have run it yet. With nothing queued to the word that also holds for
// equal and mask acquires: the back can't change the word before the front sends a release.
// EDEN_NX2_ACQ_NO_DRAIN=1 turns on: equal and mask acquires are checked the same way (with
// nothing queued to the word, memory decides; else the last release queued to it), and an acquire
// nothing queued can meet suspends its channel without draining (Puller::PollAcquire). Opt-in:
// Donkey Kong Bananza froze with it when breaking walls, both channels suspended for good.
bool AcquireNoDrain() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_ACQ_NO_DRAIN");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

// EDEN_NX2_ACQ_BACK_CHECK=1: an equal or mask acquire suspends its channel without a drain, and
// the back registers the waiter and reads the word where it reaches the acquire. Only that value
// and the writes after it resume the channel; the front never decides from its own read, which
// can be a value an earlier round left (Donkey Kong Bananza's flag words cycle 1, 2, 3 and
// 0xffffffff, 0, 2 every frame). Returns the word's device address when it applies.
std::optional<DAddr> BackCheckDevice(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire) {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_ACQ_BACK_CHECK");
        return value != nullptr && value[0] == '1';
    }();
    auto& split = VideoCommon::GpuSplit::Get();
    if (!on || acquire.WaitsToReach() || !split.Active() || !split.FrontMacros() ||
        !ReleasesLandAtOnce()) {
        return std::nullopt;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    // A word the buffer cache may hold newer than memory, or one a release the front sent may
    // still write: drain as before.
    if (!device || Nx2MayBeGpuWritten(*device, sizeof(u32)) ||
        split.QueuedSemaphoreWriteAt(*device)) {
        return std::nullopt;
    }
    return *device;
}

AcquireCheck CheckWithoutDrain(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire,
                               AcquireDrainReason& reason) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!acquire.WaitsToReach() && !AcquireNoDrain()) {
        reason = AcquireDrainReason::NotGequal;
        return AcquireCheck::Drain;
    }
    if (!split.Active() || !split.FrontMacros() || !ReleasesLandAtOnce()) {
        reason = AcquireDrainReason::Setup;
        return AcquireCheck::Drain;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
    // A word the buffer cache may hold newer than memory is read through the back.
    if (!device || Nx2MayBeGpuWritten(*device, sizeof(u32))) {
        reason = AcquireDrainReason::GpuWritten;
        return AcquireCheck::Drain;
    }
    if (const auto queued = split.QueuedSemaphoreWriteAt(*device)) {
        if (queued->known && acquire.Matches(queued->payload)) {
            return AcquireCheck::MetByQueued;
        }
        reason = queued->known ? AcquireDrainReason::QueuedLower : AcquireDrainReason::QueuedUnknown;
        return AcquireCheck::Drain;
    }
    if (acquire.Matches(dma_pusher.memory_manager.Read<u32>(acquire.address))) {
        return AcquireCheck::Met;
    }
    reason = AcquireDrainReason::NotMet;
    return AcquireCheck::NotMet;
}

} // Anonymous namespace

bool Puller::PendingAcquire::WaitsToReach() const {
    return static_cast<GpuSemaphoreOperation>(operation) == GpuSemaphoreOperation::AcquireGequal;
}

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
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Add(Profile::Counter::AcquirePolls, 1);
    const bool met = PollAcquire(dma_pusher);
    if (met) {
        Profile::Add(Profile::Counter::AcquirePollsMet, 1);
        if (Nx2PendingWake::verify_resume) [[unlikely]] {
            // pending_wake (Verify): a resume after a wake must still be met or latched.
            DrainBack(VideoCommon::DrainSite::AcquirePoll);
            const u32 word = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
            if (!pending_acquire->Matches(word) && !Nx2SemaphoreWaiters::Get().Latched(this)) {
                const u64 count = Nx2PendingWake::verify_unmet.fetch_add(1) + 1;
                if (count <= 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf pending_wake: resumed unmet: channel {} semaphore {:#x} "
                              "(op {}, {:#x}, now {:#x}) ({} differences)",
                              dma_pusher.channel_state.bind_id, pending_acquire->address,
                              pending_acquire->operation, pending_acquire->value, word, count);
                }
            }
        }
    }
    return met;
}

bool Puller::PollAcquire(DmaPusher& dma_pusher) {
    // A latched release resumes the channel whatever the word holds now: only the log below
    // reads it, so drain for it only while that log is still printing.
    static std::atomic<u32> rescued{};
    if (Nx2SemaphoreWaiters::Get().Latched(this) && rescued.load(std::memory_order_relaxed) >= 64) {
        return true;
    }
    // Checked on the back: resumed only by its latch. After 32 ms, drain and read as before (the
    // drain also runs the back's check).
    if (pending_acquire->back_check && !pending_acquire->fell_back) {
        if (Nx2SemaphoreWaiters::Get().Latched(this)) {
            return true;
        }
        if (std::chrono::steady_clock::now() - pending_acquire->since <
            std::chrono::milliseconds{32}) {
            return false;
        }
        pending_acquire->fell_back = true;
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireDrainFallbacks, 1);
    }
    // Suspended without a drain: the word changes only once a release is sent to it (or the CPU
    // writes it). Drain as before once that took 2 ms, in case a writer the front can't see does.
    if (pending_acquire->no_drain && !pending_acquire->back_check &&
        !Nx2SemaphoreWaiters::Get().Latched(this) &&
        std::chrono::steady_clock::now() - pending_acquire->since < std::chrono::milliseconds{2}) {
        AcquireDrainReason reason{};
        switch (CheckWithoutDrain(dma_pusher, *pending_acquire, reason)) {
        case AcquireCheck::Met:
            return true;
        case AcquireCheck::MetByQueued:
            VideoCommon::GpuSplit::Get().NoteSync();
            return true;
        case AcquireCheck::NotMet:
            return false;
        case AcquireCheck::Drain:
            break;
        }
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    DrainBack(VideoCommon::DrainSite::AcquirePoll);
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (!pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
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
    // every channel waiting none comes: release them again now and then while waiting.
    const bool delayed = Settings::IsGPUFenceBehaviorDefault()
                             ? Settings::IsGPULevelHigh()
                             : Settings::IsGPUFenceBehaviorBalanced() ||
                                   Settings::IsGPUFenceBehaviorAccurate();
    if (const auto now = std::chrono::steady_clock::now();
        delayed && now - pending_acquire->last_fences >= std::chrono::milliseconds{1}) {
        pending_acquire->last_fences = now;
        ReleaseFences(dma_pusher);
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
        // Who should have written it: GPU writes seen while suspended (none: the CPU, a write
        // path that doesn't latch waiters, or work that never ran).
        const auto [writes, last_written] = Nx2SemaphoreWaiters::Get().Writes(this);
        LOG_WARNING(HW_GPU, "Channel {} stuck acquire: {} GPU writes to the word while suspended "
                    "(last {:#x}), suspended {}",
                    dma_pusher.channel_state.bind_id, writes, last_written,
                    pending_acquire->back_check ? "for a check on the back"
                    : pending_acquire->no_drain ? "without a drain"
                                                : "after a drain");
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those made the count odd ("suspended") at random, so submit-time snapshots were
    // thrown away for about half of all entries (CommandList::snapshot_suspensions).
    if (pending_acquire) {
        if (const u64 watch = Nx2SemWatchDevice(); watch != 0) [[unlikely]] {
            const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
            if (device && (*device & ~u64{3}) == watch && Nx2SemWatchLog()) {
                LOG_INFO(HW_GPU, "Sem watch: channel {} resumes after {} us, {}, memory {:#x}",
                         dma_pusher.channel_state.bind_id,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - pending_acquire->since)
                             .count(),
                         Nx2SemaphoreWaiters::Get().Latched(this) ? "latched by a GPU write"
                                                                    : "by a memory read",
                         dma_pusher.memory_manager.Read<u32>(pending_acquire->address));
            }
        }
        if (pending_acquire->stats) [[unlikely]] {
            AcquireStats::Get().NoteResumed(
                pending_acquire->address,
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - pending_acquire->since)
                                     .count()),
                Nx2SemaphoreWaiters::Get().Latched(this));
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
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
               VideoCommon::GpuSplit::Effect::None);
        VideoCommon::GpuSplit::Get().NoteSync();
        break;
    case Puller::FenceOperation::Increment:
        OnBack(
            [rasterizer = dma_pusher.rasterizer,
             id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
                rasterizer->SignalSyncPoint(id);
            },
            VideoCommon::GpuSplit::Effect::None);
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
            VideoCommon::GpuSplit::Get().NoteWrite(dma_pusher.memory_manager, mirror, 16);
        }
        OnBackRelease(dma_pusher, sequence_address,
                      [rasterizer = dma_pusher.rasterizer, sequence_address, mirror, properties, payload] {
                          rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
                          if (mirror != 0) {
                              rasterizer->Query(mirror, VideoCommon::QueryType::Payload, properties, payload, 0);
                          }
                      });
        const bool four_words = properties == VideoCommon::QueryPropertiesFlags::HasTimeout;
        NoteRelease(dma_pusher, sequence_address, payload, four_words);
        if (mirror != 0) {
            NoteRelease(dma_pusher, mirror, payload, four_words);
        }
        // After the release's entry was sent, so its position covers it.
        if (auto& split = VideoCommon::GpuSplit::Get(); mirror != 0 && split.Active()) {
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(mirror)) {
                split.NoteAsyncWrite(*device, 16);
            }
        }
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
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
        const bool watched = [&] {
            const u64 watch = Nx2SemWatchDevice();
            const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
            return watch != 0 && device && (*device & ~u64{3}) == watch;
        }();
        if (watched && Nx2SemWatchLog()) [[unlikely]] {
            LOG_INFO(HW_GPU, "Sem watch: channel {} acquire op {} value {:#x} at {:#x}, memory {:#x}",
                     dma_pusher.channel_state.bind_id, acquire.operation, acquire.value,
                     acquire.address, dma_pusher.memory_manager.Read<u32>(acquire.address));
        }
        AcquireDrainReason drain_reason{};
        if (const AcquireCheck without_drain = CheckWithoutDrain(dma_pusher, acquire, drain_reason);
            without_drain == AcquireCheck::Met || without_drain == AcquireCheck::MetByQueued) {
            if (without_drain == AcquireCheck::MetByQueued) {
                // What the front reads after the acquire waits for the work sent before the
                // release (GpuSplit::NoteSync).
                VideoCommon::GpuSplit::Get().NoteSync();
            }
            return;
        }
        if (const auto device = BackCheckDevice(dma_pusher, acquire)) {
            pending_acquire = acquire;
            pending_acquire->since = std::chrono::steady_clock::now();
            pending_acquire->no_drain = true;
            pending_acquire->back_check = true;
            OnBack(
                [this, &memory_manager = dma_pusher.memory_manager, address = acquire.address,
                 device = *device, value = acquire.value, operation = acquire.operation] {
                    Nx2SemaphoreWaiters::Get().AddChecked(this, device, value, operation,
                                                          memory_manager.Read<u32>(address));
                },
                VideoCommon::GpuSplit::Effect::None);
            if (AcquireStats::On()) [[unlikely]] {
                pending_acquire->stats = true;
                AcquireStats::Get().NoteSuspended(acquire.address, false);
            }
            if (watched && Nx2SemWatchLog()) [[unlikely]] {
                LOG_INFO(HW_GPU, "Sem watch: channel {} suspended for a check on the back",
                         dma_pusher.channel_state.bind_id);
            }
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
            return;
        }
        if (AcquireNoDrain() && drain_reason == AcquireDrainReason::NotMet) {
            // Nothing sent can meet it yet: suspend at once, so the front parses the channel
            // that releases it while the back runs the queued work.
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
                Nx2SemaphoreWaiters::Get().Add(this, *device, acquire.value, acquire.operation);
            }
            pending_acquire = acquire;
            pending_acquire->since = std::chrono::steady_clock::now();
            pending_acquire->no_drain = true;
            if (AcquireStats::On()) [[unlikely]] {
                pending_acquire->stats = true;
                AcquireStats::Get().NoteSuspended(acquire.address, false);
            }
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
            return;
        }
        const u64 drain_start = VideoCommon::Nx2Profile::Enabled() ? VideoCommon::Nx2Profile::Now() : 0;
        const auto stats_start = std::chrono::steady_clock::now();
        DrainBack(VideoCommon::DrainSite::AcquireTrigger);
        const u32 drained = ReadSemaphoreWord(dma_pusher, acquire.address);
        const bool met_after_drain = acquire.Matches(drained);
        if (watched && Nx2SemWatchLog()) [[unlikely]] {
            LOG_INFO(HW_GPU, "Sem watch: channel {} after the trigger drain {:#x} ({}, reason {})",
                     dma_pusher.channel_state.bind_id, drained,
                     met_after_drain ? "met" : "not met", static_cast<u32>(drain_reason));
        }
        if (AcquireStats::On()) [[unlikely]] {
            AcquireStats::Get().NoteDrain(
                acquire.address, acquire.operation, dma_pusher.channel_state.bind_id,
                acquire.value, drained,
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - stats_start)
                                     .count()));
        }
        if (drain_start != 0) {
            using VideoCommon::Nx2Profile::Counter;
            const u32 index = static_cast<u32>(drain_reason);
            VideoCommon::Nx2Profile::Add(
                static_cast<Counter>(static_cast<u32>(Counter::AcquireDrainNotGequal) + index), 1);
            VideoCommon::Nx2Profile::Add(
                static_cast<Counter>(static_cast<u32>(Counter::AcquireDrainNotGequalNs) + index),
                VideoCommon::Nx2Profile::Now() - drain_start);
            if (met_after_drain) {
                VideoCommon::Nx2Profile::Add(
                    static_cast<Counter>(static_cast<u32>(Counter::AcquireDrainNotGequalMet) +
                                         index),
                    1);
            }
        }
        if (met_after_drain) {
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        ReleaseFences(dma_pusher);
        if (!waiters.Latched(this) && !acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            pending_acquire = acquire;
            if (AcquireStats::On()) [[unlikely]] {
                pending_acquire->since = std::chrono::steady_clock::now();
                pending_acquire->stats = true;
                const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
                AcquireStats::Get().NoteSuspended(
                    acquire.address,
                    device && VideoCommon::GpuSplit::Get().QueuedSemaphoreWriteAt(*device));
            }
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
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    ReleaseFences(dma_pusher);
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
    OnBackRelease(dma_pusher, sequence_address,
                  [rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
                      rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                                        VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
                  });
    NoteRelease(dma_pusher, sequence_address, payload, false);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack();
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        ReleaseFences(dma_pusher);
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
    if (operation == 1) { // RELEASE
        OnBackRelease(dma_pusher, address, [rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        NoteRelease(dma_pusher, address, payload, false);
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    // The check only feeds the warning below: drain for it during the first checks only.
    static std::atomic<u32> reported{};
    static std::atomic<u32> checks{};
    if (reported.load(std::memory_order_relaxed) >= 8 ||
        checks.fetch_add(1, std::memory_order_relaxed) >= 256) {
        return;
    }
    DrainBack();
    ReleaseFences(dma_pusher);
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    const bool ready = operation == 0   ? word == payload
                       : operation == 2 ? word >= payload
                       : operation == 3 ? static_cast<s32>(word - payload) >= 0
                       : operation == 4 ? (word & payload) != 0
                       : operation == 5 ? (word & payload) == 0
                                        : true;
    if (!ready) {
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
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); },
               VideoCommon::GpuSplit::Effect::None);
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); },
               VideoCommon::GpuSplit::Effect::None);
        VideoCommon::GpuSplit::Get().NoteSync();
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
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); },
               VideoCommon::GpuSplit::Effect::None);
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
