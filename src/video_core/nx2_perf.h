// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

// EXPERIMENT (not for nx2): GPU back-thread optimizations for Ocarina of Time.
// split_overlap is quarantined; the other optimizations are on by default.
// EDEN_NX2_PERF_OPT=0 turns every one off for the A/B; EDEN_NX2_PERF_ON=<name>,... turns an
// experimental one on; EDEN_NX2_PERF_OFF=<name>,... turns off
// only those named (Opt below). EDEN_NX2_PERF_VERIFY=1 computes each optimized answer the old
// way too, logs any difference and uses the old answer.
namespace VideoCommon::Nx2Perf {

enum class Opt : unsigned {
    Bounds,            ///< MaxContinuousRange / GetMemoryLayoutSize from map-time runs
    Method,            ///< Maxwell3D fast method dispatch on the back
    CbBatch,           ///< Consecutive constant buffer words uploaded in one write
    CleanWords,        ///< SynchronizeBuffer skips clean tracker words
    IndexRebind,       ///< Repeated vkCmdBindIndexBuffer skipped
    UsageTracker,      ///< UsageTracker::Track skips pages known to be used
    BankCheck,         ///< Native 64 KiB banks: per-frame set checked first
    SplitOverlap,      ///< Quarantined: pending writes are not a proof of safe early MME reads
    FrontSleep,        ///< The split front sleeps instead of yielding while it waits
    SubmitBudget,      ///< 4096-draw native submission budget outside MoltenVK
    FragmentBarrier,   ///< FragmentBarrier deferred to the next draw that reads the pass
    FenceFlush,        ///< 5 ms DeferFlush, command list copies submitted at once
    FramesAhead,       ///< Ocarina presents a frame ahead
    PipelineRetention, ///< Sixteen sessions of used pipelines, saved every 15 s
    WriteIntersections,
    SortedPageBatches,
    QmdScratch,
    QueryDiagnostics,
    ImmediateCallbacks,
    QueryUploadScratch,
    MacroReadEarly, ///< MME DMA reads outside GPU-written 1 MiB regions skip the full drain
    // Experimental, off until a PERF_VERIFY run shows zero mismatches: EDEN_NX2_PERF_ON=<name> turns one on.
    AcquireEarly,       ///< Semaphore acquires check the word without a full drain of the back
    MacroReadCbOverlay, ///< Early MME reads overlay queued constant buffer words instead of waiting
    MacroReadWriterSet, ///< Early MME reads wait only for entries that can write memory
    PreflushAging,      ///< Preflush marks the CPU stopped reading age out
    EagerPublish,       ///< The front publishes ring entries while the back is idle, and at writers
    FrontSameFirst,     ///< Split front: same-value state writes skipped before any other work
    FrontClassTable,    ///< Split front: executable method classes from one precomputed table
    FrontRun,           ///< Split front: a command's direct 3D register words written in one call
    MacroBatch,  ///< Front macro writes buffered in the JIT, same-value ones dropped inline
    MacroBorrow, ///< Front macros read their parameters from the command words in place
    MacroState,  ///< The front macro JIT keeps its State across calls
    DownloadBatch,      ///< Fence-time downloads: one copy per buffer, ranges <= 4 KiB apart merged
    DispatchBatch,      ///< Ampere draws dispatch the scheduler every 128 draws instead of 32
    PipelineFastCache,  ///< Direct-mapped graphics pipeline cache in front of the slow path
    DrawPreamble,       ///< GPU::TickWork skips the sync mutex when no request is queued
    BindMemo,           ///< Unchanged non-written binds skip touch, sync and usage across draws
    /// With MacroReadCbOverlay and MacroReadWriterSet: the overlay waits only for writers, not
    /// for queued proven draws, and the write tracker keeps write kinds apart
    MacroReadOverlayWriters,
    /// Split back, Ampere: submits recorded draws while the front waits for the back or the
    /// host GPU has run dry, so a command list copy waits only for the tail behind it
    SubmitWhileWaited,
    GpuCorePin,         ///< The GPU front and back get a reserved V-Cache core (EDEN_NX2_GPU_PIN)
    /// Ocarina (frames ahead): a present waits only for the previous frame, also when the GPU
    /// threads are behind
    PresentOneAhead,
    PresentGuestWait,   ///< vi2's present fence wait sleeps in the guest kernel, not on a host cv
    /// Split back: a front waiting at a non-list site (macro DMA writes, early acquire, map)
    /// sees its position consumed before the back runs its idle hook
    ReleaseBeforeHook,
    /// Semaphore acquire triggers take an early word only when it meets the value; an unmet
    /// early word falls through to the drained decision (independent of AcquireEarly)
    AcquireTriggerEarly,
    /// Pipeline eviction keeps the ones built at boot that this session has not drawn with yet
    PipelineKeepPreloaded,
    /// Split front: an executable draw-manager method that only draws is a writer only when
    /// its bound programs are not proven non-writing (MacroReadWriterSet), not always
    WriterSetExec,
    /// Noted writes keep their exact bytes; early MME DMA reads and semaphore reads wait only
    /// for noted writes to the bytes they read, not to their whole page
    ExactNotedWrites,
    /// Guest CPU cores and HostTiming run on the non-V-Cache CCD; the GPU threads keep the
    /// V-Cache CCD (asymmetric L3 with V-Cache pinning only)
    GuestCcd,
    /// Native 64 KiB uniform banks: guest bytes compared with the shadow on the frame's first
    /// bind only, not on every draw (a later CPU rewrite in the frame shows one frame late)
    BankOncePerFrame,
    /// The host syncpoint value rises only once everything recorded before the increment
    /// (downloads, query results, fence operations) has landed in guest memory; vi2's early
    /// returns on the frames-ahead path wait for the previous frame's landed value
    LandedPrevAlways,
    /// With PresentOneAhead: a present that skips this frame's wait first waits for the
    /// previous frame's landed (host) syncpoint value
    OneAheadLanded,
    /// Split front: queued constant buffer words, noted writes and draw epochs in preallocated
    /// rings instead of std::deque (one heap block per element on MSVC)
    SplitRings,
    /// With BindMemo: a memo hit is decided by the bound range's own CPU-modified bits and a
    /// buffer create/delete epoch, not by the global tracker epochs; a new scheduler tick only
    /// redoes the usage mark
    BindMemoLocal,
    /// GPUDirtyMemoryManager::Gather returns without its locks when nothing was collected
    DirtyGatherFast,
    /// Texture handles of one draw stage read through one translated constant buffer page,
    /// not a GPU page walk per handle
    HandlePageCache,
    /// Split front: a native macro call whose MME DMA read only feeds its last writes (render
    /// enable) runs without waiting for the back; the back writes those from the real words
    DmaForward,
    /// Split back: its idle hook submits deferred work only when the back sleeps, someone asked,
    /// the front waits for GPU results, or the deferred submit is older than
    /// EDEN_NX2_HOOK_FLUSH_US (default 1000), not at every catch-up
    HookLazyFlush,
    Count,
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Opt::Count)> opt_names{
    "bounds",         "method",          "cb_batch",    "clean_words",   "index_rebind",
    "usage_tracker",  "bank_check",      "split_overlap", "front_sleep", "submit_budget",
    "fragment_barrier", "fence_flush",   "frames_ahead", "pipeline_retention",
    "write_intersections", "sorted_page_batches",
    "qmd_scratch", "query_diagnostics", "immediate_callbacks", "query_upload_scratch",
    "macro_read_early",
    "acquire_early", "macro_read_cb_overlay", "macro_read_writer_set", "preflush_aging",
    "eager_publish",
    "front_same_first", "front_class_table", "front_run",
    "macro_batch", "macro_borrow", "macro_state",
    "download_batch",
    "dispatch_batch", "pipeline_fast_cache", "draw_preamble", "bind_memo",
    "macro_read_overlay_writers",
    "submit_while_waited",
    "gpu_core_pin",
    "present_one_ahead", "present_guest_wait", "release_before_hook", "acquire_trigger_early",
    "pipeline_keep_preloaded",
    "writer_set_exec",
    "exact_noted_writes",
    "guest_ccd",
    "bank_once_per_frame",
    "landed_prev_always", "one_ahead_landed",
    "split_rings",
    "bind_memo_local",
    "dirty_gather_fast",
    "handle_page_cache",
    "dma_forward",
    "hook_lazy_flush",
};

static_assert(static_cast<size_t>(Opt::Count) <= 64, "the off-mask is 64 bits");
static_assert(static_cast<unsigned>(Opt::DispatchBatch) == 33 &&
              static_cast<unsigned>(Opt::BindMemo) == 36 &&
              static_cast<unsigned>(Opt::WriterSetExec) == 45 &&
              static_cast<unsigned>(Opt::ExactNotedWrites) == 46 &&
              static_cast<unsigned>(Opt::GuestCcd) == 47 &&
              static_cast<unsigned>(Opt::BankOncePerFrame) == 48 &&
              static_cast<unsigned>(Opt::LandedPrevAlways) == 49 &&
              static_cast<unsigned>(Opt::OneAheadLanded) == 50 &&
              static_cast<unsigned>(Opt::SplitRings) == 51 &&
              static_cast<unsigned>(Opt::BindMemoLocal) == 52);

constexpr u64 Bit(Opt opt) {
    return u64{1} << static_cast<unsigned>(opt);
}

/// Bit i of an off-mask is opt_names[i]: set means off.
constexpr bool IsOff(u64 mask, size_t index) {
    return ((mask >> index) & 1) != 0;
}

/// Off unless EDEN_NX2_PERF_ON names them: quarantined or not yet verified.
inline constexpr u64 DefaultOffMask() {
    u64 mask = 0;
    for (const Opt opt : {Opt::SplitOverlap, Opt::AcquireEarly, Opt::MacroReadCbOverlay,
                          Opt::MacroReadWriterSet, Opt::PreflushAging, Opt::EagerPublish,
                          Opt::FrontSameFirst, Opt::FrontClassTable, Opt::FrontRun,
                          Opt::MacroBatch, Opt::MacroBorrow, Opt::MacroState, Opt::DownloadBatch,
                          Opt::DispatchBatch, Opt::PipelineFastCache, Opt::DrawPreamble,
                          Opt::BindMemo, Opt::MacroReadOverlayWriters,
                          Opt::SubmitWhileWaited, Opt::GpuCorePin, Opt::PresentOneAhead,
                          Opt::PresentGuestWait,
                          Opt::ReleaseBeforeHook,
                          Opt::AcquireTriggerEarly, Opt::PipelineKeepPreloaded,
                          Opt::WriterSetExec, Opt::ExactNotedWrites, Opt::GuestCcd, Opt::DmaForward,
                          Opt::HookLazyFlush,
                          Opt::BankOncePerFrame, Opt::LandedPrevAlways, Opt::OneAheadLanded,
                          Opt::SplitRings, Opt::BindMemoLocal}) {
        mask |= Bit(opt);
    }
    return mask;
}

/// Off unless named in EDEN_NX2_PERF_ON: split_overlap (quarantined); acquire_early (its early
/// reads see "not met" where a drain sees "met", so channels wait up to 2 ms longer);
/// dispatch_batch (delays GPU work; suspected of the flashes, no verify mode);
/// macro_read_overlay_writers (+0.3 FPS, noise); gpu_core_pin (pair -2.3 FPS, separate +0.4);
/// present_guest_wait (guest panic at load in a speed run); present_one_ahead (+9 FPS, but the
/// guest rewrites constants the back has not read yet: incessant brightness flashes in play);
/// frames_ahead (same hazard: the lab's flash check sees 25-40% of frames at another brightness
/// with it on, 0 with it off; ~5 ms/frame slower at the Kokiri spawn without it);
/// release_before_hook (+0.0 FPS);
/// acquire_trigger_early (+0.2 FPS); pipeline_keep_preloaded, writer_set_exec,
/// exact_noted_writes, guest_ccd, landed_prev_always, one_ahead_landed, split_rings,
/// bind_memo_local, hook_lazy_flush (not measured yet).
inline constexpr u64 always_off = Bit(Opt::SplitOverlap) | Bit(Opt::AcquireEarly) |
                                  Bit(Opt::DispatchBatch) | Bit(Opt::MacroReadOverlayWriters) |
                                  Bit(Opt::GpuCorePin) | Bit(Opt::PresentGuestWait) |
                                  Bit(Opt::PresentOneAhead) | Bit(Opt::FramesAhead) |
                                  Bit(Opt::ReleaseBeforeHook) |
                                  Bit(Opt::AcquireTriggerEarly) |
                                  Bit(Opt::PipelineKeepPreloaded) | Bit(Opt::WriterSetExec) |
                                  Bit(Opt::ExactNotedWrites) | Bit(Opt::GuestCcd) |
                                  Bit(Opt::DmaForward) | Bit(Opt::HookLazyFlush) |
                                  Bit(Opt::LandedPrevAlways) | Bit(Opt::OneAheadLanded) |
                                  Bit(Opt::SplitRings) | Bit(Opt::BindMemoLocal);

static_assert(IsOff(always_off, 33));
static_assert(static_cast<unsigned>(Opt::PipelineKeepPreloaded) == 44 && IsOff(always_off, 44));
static_assert(IsOff(always_off, 45) && IsOff(DefaultOffMask(), 45));
static_assert(IsOff(DefaultOffMask(), 36));
static_assert(!IsOff(DefaultOffMask(), 0));
static_assert(IsOff(always_off, 49) && IsOff(DefaultOffMask(), 49) && IsOff(always_off, 50) &&
              IsOff(DefaultOffMask(), 50));
static_assert(IsOff(always_off, 47) && IsOff(DefaultOffMask(), 47));
// bank_once_per_frame is on by default; SAFE turns it off (suspected of the flashes).
static_assert(!IsOff(always_off, 48) && IsOff(DefaultOffMask(), 48));
static_assert(IsOff(always_off, 51) && IsOff(DefaultOffMask(), 51));
static_assert(IsOff(always_off, 52) && IsOff(DefaultOffMask(), 52));
static_assert(IsOff(always_off, static_cast<size_t>(Opt::HookLazyFlush)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::HookLazyFlush)));

/// The off-mask from EDEN_NX2_PERF_SAFE (safe), EDEN_NX2_PERF_ON (on), EDEN_NX2_PERF_OFF (off)
/// and EDEN_NX2_PERF_FRAMES_AHEAD=0 (frames_ahead_off).
inline u64 ParseOffMask(bool safe, std::string_view on, std::string_view off,
                        bool frames_ahead_off) {
    // Every candidate is on for the user's play test; EDEN_NX2_PERF_SAFE=1 puts the experimental
    // set back to off (split_overlap stays quarantined either way).
    u64 mask = safe ? DefaultOffMask() | always_off : always_off;
    while (!on.empty()) {
        const size_t comma = on.find(',');
        const std::string_view name = on.substr(0, comma);
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask &= ~(u64{1} << i);
            }
        }
        on = comma == std::string_view::npos ? std::string_view{} : on.substr(comma + 1);
    }
    if (frames_ahead_off) {
        mask |= Bit(Opt::FramesAhead);
    }
    while (!off.empty()) {
        const size_t comma = off.find(',');
        const std::string_view name = off.substr(0, comma);
        bool known = false;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask |= u64{1} << i;
                known = true;
            }
        }
        if (!known && !name.empty()) {
            LOG_WARNING(HW_GPU, "Nx2Perf: unknown name {} in EDEN_NX2_PERF_OFF", name);
        }
        off = comma == std::string_view::npos ? std::string_view{} : off.substr(comma + 1);
    }
    return mask;
}

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_OPT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// Live switches (EDEN_NX2_PERF_CONTROL, nx2_perf.cpp). New switches are not live-safe until a
/// call-site audit shows none caches On() or keeps state the other branch does not handle.
/// Switches without an On() call site (bank_check, write_intersections, immediate_callbacks)
/// are left out so a live flip of them is reported as ignored instead of measuring nothing.
inline constexpr u64 live_safe =
    Bit(Opt::CbBatch) | Bit(Opt::CleanWords) | Bit(Opt::UsageTracker) | Bit(Opt::FrontSleep) |
    Bit(Opt::FragmentBarrier) | Bit(Opt::FramesAhead) | Bit(Opt::SortedPageBatches) |
    Bit(Opt::QueryDiagnostics) | Bit(Opt::QueryUploadScratch) | Bit(Opt::FrontRun) |
    Bit(Opt::DownloadBatch) | Bit(Opt::AcquireTriggerEarly) | Bit(Opt::PipelineKeepPreloaded);

/// One control file: the whole arm, computed from the startup off-mask.
struct ControlResult {
    u64 mask;
    std::string label;
    u64 on_vs_startup;
    u64 off_vs_startup;
    std::string ignored; ///< Names not live-safe or unknown and unknown directives, comma list
};
ControlResult ParseControl(u64 startup_mask, std::string_view text, u64 live_safe_mask);
/// "set=<label> on=<names|-> off=<names|-> ignored=<names|-> mask=0x<16 hex>"
std::string FormatControl(const ControlResult& result, u64 effective_mask);

namespace detail {
/// Off-mask in effect (bit set = off; every switch when EDEN_NX2_PERF_OPT=0); ~0 before the
/// first On(). The live poller stores into it.
inline std::atomic<u64> live_off_mask{~u64{0}};
u64 InitOffMask();
} // namespace detail

inline bool On(Opt opt) {
    u64 off = detail::live_off_mask.load(std::memory_order_relaxed);
    if (off == ~u64{0}) [[unlikely]] {
        off = detail::InitOffMask();
    }
    return !IsOff(off, static_cast<size_t>(opt));
}

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

/// landed_prev_always / one_ahead_landed: the host syncpoint value rises on the fencing thread,
/// after everything recorded before it has landed. Also under EDEN_NX2_PERF_VERIFY, so vi2 can
/// report presents that return before the previous frame landed.
inline bool LandedSyncpoints() {
    static const bool landed =
        On(Opt::LandedPrevAlways) || On(Opt::OneAheadLanded) || Verify();
    return landed;
}

} // namespace VideoCommon::Nx2Perf
