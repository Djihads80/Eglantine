// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

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
};

/// Off unless EDEN_NX2_PERF_ON names them: quarantined or not yet verified.
inline constexpr unsigned DefaultOffMask() {
    unsigned mask = 0;
    for (const Opt opt : {Opt::SplitOverlap, Opt::AcquireEarly, Opt::MacroReadCbOverlay,
                          Opt::MacroReadWriterSet, Opt::PreflushAging, Opt::EagerPublish,
                          Opt::FrontSameFirst, Opt::FrontClassTable, Opt::FrontRun,
                          Opt::MacroBatch, Opt::MacroBorrow, Opt::MacroState, Opt::DownloadBatch,
                          Opt::DispatchBatch, Opt::PipelineFastCache, Opt::DrawPreamble,
                          Opt::BindMemo}) {
        mask |= 1U << static_cast<unsigned>(opt);
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

inline bool On(Opt opt) {
    static const unsigned off = [] {
        // Every candidate is on for the user's play test; EDEN_NX2_PERF_SAFE=1 puts the experimental
        // set back to off (split_overlap stays quarantined either way).
        unsigned mask = std::getenv("EDEN_NX2_PERF_SAFE") != nullptr
                            ? DefaultOffMask()
                            : 1U << static_cast<unsigned>(Opt::SplitOverlap);
        std::string_view on_list = std::getenv("EDEN_NX2_PERF_ON") ? std::getenv("EDEN_NX2_PERF_ON") : "";
        while (!on_list.empty()) {
            const size_t comma = on_list.find(',');
            const std::string_view name = on_list.substr(0, comma);
            for (size_t i = 0; i < opt_names.size(); ++i) {
                if (name == opt_names[i]) {
                    mask &= ~(1U << i);
                }
            }
            on_list = comma == std::string_view::npos ? std::string_view{} : on_list.substr(comma + 1);
        }
        if (const char* value = std::getenv("EDEN_NX2_PERF_FRAMES_AHEAD"); value && value[0] == '0') {
            mask |= 1U << static_cast<unsigned>(Opt::FramesAhead);
        }
        std::string_view list = std::getenv("EDEN_NX2_PERF_OFF") ? std::getenv("EDEN_NX2_PERF_OFF") : "";
        while (!list.empty()) {
            const size_t comma = list.find(',');
            const std::string_view name = list.substr(0, comma);
            bool known = false;
            for (size_t i = 0; i < opt_names.size(); ++i) {
                if (name == opt_names[i]) {
                    mask |= 1U << i;
                    known = true;
                }
            }
            if (!known && !name.empty()) {
                LOG_WARNING(HW_GPU, "Nx2Perf: unknown name {} in EDEN_NX2_PERF_OFF", name);
            }
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        }
        std::string state;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            state += fmt::format(" {}={}", opt_names[i], Enabled() && (mask >> i & 1) == 0 ? "on" : "off");
        }
        LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
        return mask;
    }();
    return Enabled() && (off >> static_cast<unsigned>(opt) & 1) == 0;
}

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

} // namespace VideoCommon::Nx2Perf
