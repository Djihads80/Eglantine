// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
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
    /// Guest-visible syncpoint values rise on the fencing thread with the host value, once
    /// everything recorded before the increment has landed in guest memory
    GuestLandedSyncpoints,
    /// Measurement only: after vi2's Ocarina reply, write-protects CPU-written GPU pages and the
    /// native uniform bank pages to time the guest's first access of them (nx2_war_probe.h)
    WarGuardProbe,
    /// Split front: an instance loop's draw.begin right after the same one, still unpublished,
    /// is counted into that ring entry (bits 24-31); the back adds the instances
    DrawBeginCoalesce,
    /// Split back: its idle hook submits deferred work only when the back sleeps, someone asked,
    /// the front waits for GPU results, or the deferred submit is older than
    /// EDEN_NX2_HOOK_FLUSH_US (default 1000), not at every catch-up
    HookLazyFlush,
    /// With frames_ahead: vi2's early reply opens an epoch for this frame's fence; the guest's
    /// first write of a page the back may still read for it waits for that fence (nx2_war_probe.h)
    InflightWriteWait,
    /// With frames_ahead: while the epoch is open, a guest read served from a preflush block whose
    /// last GPU writer is still in flight waits for that writer's host syncpoint
    InflightReadExact,
    /// Measurement only: per-syncpoint stamps of the front's parse of a fence increment, the
    /// back's increment and the back's idle time, reported by vi2's present (FenceTimeline)
    FenceTimeline,
    /// With DmaForward: a forwarded macro call's dry-run plan is cached by code generation, start,
    /// parameters and the reads it made; the ring entry names a shared code slot, not a code copy
    DmaForwardMemo,
    /// Measurement only: the device pages the split back reads per frame, which of them the guest
    /// rewrites before the next present, and how early in the frame the writers of preflushed
    /// blocks the guest reads ran (Nx2Ledger in nx2_war_probe.h)
    RunAheadLedger,
    /// Split front: a compute call whose dispatches' storage buffer writes the front can place
    /// (back-published program records, final QMD and descriptor words) notes those ranges; a
    /// command list fetch waits only for the writers that can touch its pages
    ComputeWriterRanges,
    /// Split front: a semaphore acquire is met from the payload of a release still queued in
    /// the ring when no entry queued after it can write the word's 16 bytes (no drain)
    AcquireQueuedRelease,
    /// With MacroReadWriterSet: the back skips the non-writer proof lookup when the draw's
    /// programs, program epoch and the set's version repeat its last proven draw
    NonwriterProofMemo,
    /// Split: a front wait spins (EDEN_NX2_SPIN_HANDOFF_US, default 300) while the back works
    /// instead of sleeping; the idle back spins for the next publish while the GPU thread parses
    SpinHandoff,
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
    "guest_landed_syncpoints",
    "war_guard_probe",
    "draw_begin_coalesce",
    "hook_lazy_flush",
    "inflight_write_wait", "inflight_read_exact",
    "fence_timeline",
    "dma_forward_memo",
    "run_ahead_ledger",
    "compute_writer_ranges",
    "acquire_queued_release",
    "nonwriter_proof_memo",
    "spin_handoff",
};

static_assert(static_cast<size_t>(Opt::Count) <= 128, "the off-mask is 128 bits");
static_assert(static_cast<unsigned>(Opt::DispatchBatch) == 33 &&
              static_cast<unsigned>(Opt::BindMemo) == 36 &&
              static_cast<unsigned>(Opt::WriterSetExec) == 45 &&
              static_cast<unsigned>(Opt::ExactNotedWrites) == 46 &&
              static_cast<unsigned>(Opt::GuestCcd) == 47 &&
              static_cast<unsigned>(Opt::BankOncePerFrame) == 48 &&
              static_cast<unsigned>(Opt::LandedPrevAlways) == 49 &&
              static_cast<unsigned>(Opt::OneAheadLanded) == 50 &&
              static_cast<unsigned>(Opt::SplitRings) == 51 &&
              static_cast<unsigned>(Opt::BindMemoLocal) == 52 &&
              static_cast<unsigned>(Opt::WarGuardProbe) == 57);

/// Bit i of an off-mask is opt_names[i]: set means off. Bits 0-63 in lo, 64-127 in hi.
struct OffMask {
    u64 lo{};
    u64 hi{};

    constexpr OffMask operator|(const OffMask& other) const {
        return {lo | other.lo, hi | other.hi};
    }
    constexpr OffMask operator&(const OffMask& other) const {
        return {lo & other.lo, hi & other.hi};
    }
    constexpr OffMask operator~() const {
        return {~lo, ~hi};
    }
    constexpr bool operator==(const OffMask&) const = default;
    constexpr OffMask& operator|=(const OffMask& other) {
        lo |= other.lo;
        hi |= other.hi;
        return *this;
    }
    constexpr OffMask& operator&=(const OffMask& other) {
        lo &= other.lo;
        hi &= other.hi;
        return *this;
    }
};

constexpr OffMask Bit(Opt opt) {
    const unsigned index = static_cast<unsigned>(opt);
    return index < 64 ? OffMask{u64{1} << index, 0} : OffMask{0, u64{1} << (index & 63)};
}

constexpr bool IsOff(const OffMask& mask, size_t index) {
    return (((index < 64 ? mask.lo : mask.hi) >> (index & 63)) & 1) != 0;
}

/// Off unless EDEN_NX2_PERF_ON names them: quarantined or not yet verified.
inline constexpr OffMask DefaultOffMask() {
    OffMask mask{};
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
                          Opt::HookLazyFlush, Opt::DmaForwardMemo,
                          Opt::BankOncePerFrame, Opt::LandedPrevAlways, Opt::OneAheadLanded,
                          Opt::SplitRings, Opt::BindMemoLocal, Opt::GuestLandedSyncpoints,
                          Opt::WarGuardProbe, Opt::DrawBeginCoalesce, Opt::InflightWriteWait,
                          Opt::InflightReadExact,
                          Opt::FenceTimeline, Opt::RunAheadLedger, Opt::ComputeWriterRanges, Opt::AcquireQueuedRelease, Opt::NonwriterProofMemo,
                          Opt::SpinHandoff}) {
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
/// bind_memo_local, guest_landed_syncpoints (+0.9 FPS), hook_lazy_flush (+0.9 FPS);
/// war_guard_probe (measurement, adds page traps); inflight_write_wait and inflight_read_exact
/// (timing change: flash check and a user play test first); fence_timeline (measurement); dma_forward_memo (unmeasured);
/// run_ahead_ledger (measurement); compute_writer_ranges (-2.5 FPS: many short list-writer waits),
/// acquire_queued_release (no gain, ~46 acquires/s forwarded), nonwriter_proof_memo (+0.3 FPS,
/// round 7), spin_handoff (round 8, unmeasured). dma_forward and draw_begin_coalesce are on
/// by default (round 5); SAFE turns them off.
inline constexpr OffMask always_off = Bit(Opt::SplitOverlap) | Bit(Opt::AcquireEarly) |
                                  Bit(Opt::DispatchBatch) | Bit(Opt::MacroReadOverlayWriters) |
                                  Bit(Opt::GpuCorePin) | Bit(Opt::PresentGuestWait) |
                                  Bit(Opt::PresentOneAhead) | Bit(Opt::FramesAhead) |
                                  Bit(Opt::ReleaseBeforeHook) |
                                  Bit(Opt::AcquireTriggerEarly) |
                                  Bit(Opt::PipelineKeepPreloaded) | Bit(Opt::WriterSetExec) |
                                  Bit(Opt::ExactNotedWrites) | Bit(Opt::GuestCcd) |
                                  Bit(Opt::HookLazyFlush) |
                                  Bit(Opt::LandedPrevAlways) | Bit(Opt::OneAheadLanded) |
                                  Bit(Opt::SplitRings) | Bit(Opt::BindMemoLocal) |
                                  Bit(Opt::GuestLandedSyncpoints) |
                                  Bit(Opt::WarGuardProbe) | Bit(Opt::InflightWriteWait) |
                                  Bit(Opt::InflightReadExact) |
                                  Bit(Opt::FenceTimeline) | Bit(Opt::DmaForwardMemo) |
                                  Bit(Opt::RunAheadLedger) |
                                  Bit(Opt::ComputeWriterRanges) |
                                  Bit(Opt::AcquireQueuedRelease) |
                                  Bit(Opt::NonwriterProofMemo) | Bit(Opt::SpinHandoff);

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
static_assert(static_cast<unsigned>(Opt::GuestLandedSyncpoints) == 56 && IsOff(always_off, 56) &&
              IsOff(DefaultOffMask(), 56));
static_assert(static_cast<unsigned>(Opt::WarGuardProbe) == 57 && IsOff(always_off, 57) &&
              IsOff(DefaultOffMask(), 57));
static_assert(static_cast<unsigned>(Opt::DrawBeginCoalesce) == 58 && !IsOff(always_off, 58) &&
              IsOff(DefaultOffMask(), 58));
static_assert(IsOff(always_off, static_cast<size_t>(Opt::FenceTimeline)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::FenceTimeline)));
static_assert(!IsOff(always_off, static_cast<size_t>(Opt::DmaForward)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::DmaForward)));
static_assert(IsOff(always_off, static_cast<size_t>(Opt::DmaForwardMemo)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::DmaForwardMemo)));
static_assert(IsOff(always_off, static_cast<size_t>(Opt::HookLazyFlush)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::HookLazyFlush)));
static_assert(IsOff(always_off, static_cast<size_t>(Opt::InflightWriteWait)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::InflightWriteWait)) &&
              IsOff(always_off, static_cast<size_t>(Opt::InflightReadExact)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::InflightReadExact)));
static_assert(static_cast<unsigned>(Opt::RunAheadLedger) == 64 &&
              IsOff(always_off, static_cast<size_t>(Opt::RunAheadLedger)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::RunAheadLedger)));

static_assert(static_cast<unsigned>(Opt::ComputeWriterRanges) == 65 &&
              IsOff(always_off, static_cast<size_t>(Opt::ComputeWriterRanges)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::ComputeWriterRanges)));

static_assert(static_cast<unsigned>(Opt::AcquireQueuedRelease) == 66 &&
              IsOff(always_off, static_cast<size_t>(Opt::AcquireQueuedRelease)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::AcquireQueuedRelease)));

static_assert(static_cast<unsigned>(Opt::NonwriterProofMemo) == 67 &&
              IsOff(always_off, static_cast<size_t>(Opt::NonwriterProofMemo)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::NonwriterProofMemo)));

static_assert(static_cast<unsigned>(Opt::SpinHandoff) == 68 &&
              IsOff(always_off, static_cast<size_t>(Opt::SpinHandoff)) &&
              IsOff(DefaultOffMask(), static_cast<size_t>(Opt::SpinHandoff)));

/// Switches the name front_sync_trio (an alias with no bit of its own) turns on together.
/// EDEN_NX2_PERF_OFF still turns each of them off.
inline constexpr OffMask front_sync_trio_set =
    Bit(Opt::AcquireTriggerEarly) | Bit(Opt::ExactNotedWrites) | Bit(Opt::WriterSetExec);

/// The off-mask from EDEN_NX2_PERF_SAFE (safe), EDEN_NX2_PERF_ON (on), EDEN_NX2_PERF_OFF (off)
/// and EDEN_NX2_PERF_FRAMES_AHEAD=0 (frames_ahead_off).
inline OffMask ParseOffMask(bool safe, std::string_view on, std::string_view off,
                        bool frames_ahead_off) {
    // Every candidate is on for the user's play test; EDEN_NX2_PERF_SAFE=1 puts the experimental
    // set back to off (split_overlap stays quarantined either way).
    OffMask mask = safe ? DefaultOffMask() | always_off : always_off;
    while (!on.empty()) {
        const size_t comma = on.find(',');
        const std::string_view name = on.substr(0, comma);
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask &= ~Bit(static_cast<Opt>(i));
            }
        }
        if (name == "front_sync_trio") {
            mask &= ~front_sync_trio_set;
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
                mask |= Bit(static_cast<Opt>(i));
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
inline constexpr OffMask live_safe =
    Bit(Opt::CbBatch) | Bit(Opt::CleanWords) | Bit(Opt::UsageTracker) | Bit(Opt::FrontSleep) |
    Bit(Opt::FragmentBarrier) | Bit(Opt::FramesAhead) | Bit(Opt::SortedPageBatches) |
    Bit(Opt::QueryDiagnostics) | Bit(Opt::QueryUploadScratch) | Bit(Opt::FrontRun) |
    Bit(Opt::DownloadBatch) | Bit(Opt::AcquireTriggerEarly) | Bit(Opt::PipelineKeepPreloaded);

/// One control file: the whole arm, computed from the startup off-mask.
struct ControlResult {
    OffMask mask;
    std::string label;
    OffMask on_vs_startup;
    OffMask off_vs_startup;
    std::string ignored; ///< Names not live-safe or unknown and unknown directives, comma list
};
ControlResult ParseControl(const OffMask& startup_mask, std::string_view text,
                           const OffMask& live_safe_mask);
/// "set=<label> on=<names|-> off=<names|-> ignored=<names|-> mask=0x<32 hex, hi then lo>"
std::string FormatControl(const ControlResult& result, const OffMask& effective_mask);

namespace detail {
/// Off-mask in effect, lo then hi (bit set = off; every switch when EDEN_NX2_PERF_OPT=0); both
/// words ~0 before the first On(). The live poller stores hi then lo.
inline std::array<std::atomic<u64>, 2> live_off_mask{{~u64{0}, ~u64{0}}};
OffMask InitOffMask();
} // namespace detail

inline bool On(Opt opt) {
    const size_t index = static_cast<size_t>(opt);
    u64 off = detail::live_off_mask[index >> 6].load(std::memory_order_relaxed);
    if (off == ~u64{0}) [[unlikely]] {
        // Possibly the sentinel (both words ~0). InitOffMask waits for an initialization still
        // running elsewhere and returns the live pair, so a half-stored pair is never trusted.
        const OffMask mask = detail::InitOffMask();
        off = index < 64 ? mask.lo : mask.hi;
    }
    return ((off >> (index & 63)) & 1) == 0;
}

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

/// landed_prev_always / one_ahead_landed / guest_landed_syncpoints: the host syncpoint value
/// rises on the fencing thread, after everything recorded before it has landed. Also under
/// EDEN_NX2_PERF_VERIFY, so vi2 can report presents that return before the previous frame landed.
inline bool LandedSyncpoints() {
    static const bool landed = On(Opt::LandedPrevAlways) || On(Opt::OneAheadLanded) ||
                               On(Opt::GuestLandedSyncpoints) || Verify();
    return landed;
}

/// guest_landed_syncpoints, outside Verify: the guest value rises with the landed host value.
inline bool GuestLandedSyncpoints() {
    static const bool on = On(Opt::GuestLandedSyncpoints) && !Verify();
    return on;
}

/// guest_landed_syncpoints: counts the fence manager's forced submits (deferred work sent so a
/// guest value can land) and logs their rate about once a second.
void NoteLandedForcedSubmit();

/// Where the guest observes a syncpoint value.
enum class GuestSite : u32 { QueryCounter, RoPage, SlotEvent, Vi2Wait, Count };

namespace detail {
/// EDEN_NX2_PERF_VERIFY: syncpoints the fence manager has signalled, a bit per id (192 ids).
inline std::array<std::atomic<u64>, 3> fence_syncpoints{};
} // namespace detail

/// EDEN_NX2_PERF_VERIFY: the fence manager signals syncpoint `id` (< 192).
inline void MarkFenceSyncpoint(u32 id) {
    std::atomic<u64>& word = detail::fence_syncpoints[id / 64];
    const u64 bit = u64{1} << (id % 64);
    if ((word.load(std::memory_order_relaxed) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_relaxed);
    }
}

/// EDEN_NX2_PERF_VERIFY for guest_landed_syncpoints: the guest observed syncpoint `id` at
/// `value` while its landed (host) value was `host`. Logs the first 64 values seen before they
/// landed, then per-second counts. Skips syncpoints the fence manager never signalled (only the
/// CPU raises those; their host value never moves).
void NoteGuestObservation(GuestSite site, u32 id, u32 value, u32 host);

/// fence_timeline (measurement only): when the front parsed each syncpoint increment, when the
/// back raised the guest value, when the front last popped a command list, and how long the
/// back sat idle. Relaxed stores only; vi2's present matches them to its fence wait.
namespace FenceTimeline {
inline constexpr u32 NumIds = 192;
inline constexpr u32 Depth = 8; ///< Increments of one id remembered (several per frame)

/// The last Depth stamps of one syncpoint id. One writer thread per ring.
struct Ring {
    std::array<std::atomic<s64>, Depth> ns{};
    std::array<std::atomic<u32>, Depth> seq{};   ///< Ordinal of the increment (0 = empty)
    std::array<std::atomic<u32>, Depth> value{}; ///< Back: guest value after the increment
    std::atomic<u32> count{};
};

inline std::array<Ring, NumIds> front_parse{};
inline std::array<Ring, NumIds> back_incr{};
inline std::atomic<s64> last_submit_pop_ns{};
inline std::atomic<s64> back_idle_ns_total{}; ///< Read as a difference
inline std::atomic<s64> back_idle_since_ns{}; ///< 0 while the back works

inline bool On() {
    static const bool on = Nx2Perf::On(Opt::FenceTimeline);
    return on;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Stamp(Ring& ring, s64 ns, u32 value) {
    const u32 n = ring.count.load(std::memory_order_relaxed) + 1;
    ring.count.store(n, std::memory_order_relaxed);
    const u32 slot = n % Depth;
    ring.seq[slot].store(0, std::memory_order_relaxed);
    ring.ns[slot].store(ns, std::memory_order_relaxed);
    ring.value[slot].store(value, std::memory_order_relaxed);
    ring.seq[slot].store(n, std::memory_order_relaxed);
}

/// The front parsed an increment of syncpoint `id`.
inline void StampFront(u32 id) {
    if (id < NumIds) {
        Stamp(front_parse[id], NowNs(), 0);
    }
}

/// The back raised syncpoint `id`'s guest value to `value`.
inline void StampBack(u32 id, u32 value) {
    if (id < NumIds) {
        Stamp(back_incr[id], NowNs(), value);
    }
}

inline void BeginBackIdle() {
    back_idle_since_ns.store(NowNs(), std::memory_order_relaxed);
}

inline void EndBackIdle() {
    const s64 since = back_idle_since_ns.load(std::memory_order_relaxed);
    if (since != 0) {
        back_idle_ns_total.store(back_idle_ns_total.load(std::memory_order_relaxed) + NowNs() -
                                     since,
                                 std::memory_order_relaxed);
        back_idle_since_ns.store(0, std::memory_order_relaxed);
    }
}

/// The back's idle total at `now`, counting an idle stretch still running.
inline s64 BackIdleAt(s64 now) {
    const s64 since = back_idle_since_ns.load(std::memory_order_relaxed);
    const s64 total = back_idle_ns_total.load(std::memory_order_relaxed);
    return total + (since != 0 && now > since ? now - since : 0);
}

/// Stamps of the increment that raised `id` to `value`: false if no remembered one did.
inline bool Find(u32 id, u32 value, s64& front_ns, s64& back_ns) {
    if (id >= NumIds) {
        return false;
    }
    const Ring& back = back_incr[id];
    for (u32 slot = 0; slot < Depth; ++slot) {
        const u32 n = back.seq[slot].load(std::memory_order_relaxed);
        if (n == 0 || back.value[slot].load(std::memory_order_relaxed) != value) {
            continue;
        }
        back_ns = back.ns[slot].load(std::memory_order_relaxed);
        const Ring& front = front_parse[id];
        const u32 front_slot = n % Depth;
        if (front.seq[front_slot].load(std::memory_order_relaxed) != n) {
            return false;
        }
        front_ns = front.ns[front_slot].load(std::memory_order_relaxed);
        return true;
    }
    return false;
}
} // namespace FenceTimeline

} // namespace VideoCommon::Nx2Perf
