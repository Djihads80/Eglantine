// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"

// EXPERIMENT (not for nx2): GPU back-thread optimizations for Ocarina of Time, all on by default.
// EDEN_NX2_PERF_OPT=0 turns every one off for the A/B; EDEN_NX2_PERF_OFF=<name>,... turns off
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
    SplitOverlap,      ///< MME DMA reads wait only for the queued work they depend on
    FrontSleep,        ///< The split front sleeps instead of yielding while it waits
    SubmitBudget,      ///< 4096-draw native submission budget outside MoltenVK
    FragmentBarrier,   ///< FragmentBarrier deferred to the next draw that reads the pass
    FenceFlush,        ///< 5 ms DeferFlush, command list copies submitted at once
    FramesAhead,       ///< Ocarina presents a frame ahead
    PipelineRetention, ///< Sixteen sessions of used pipelines, saved every 15 s
    Count,
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Opt::Count)> opt_names{
    "bounds",         "method",          "cb_batch",    "clean_words",   "index_rebind",
    "usage_tracker",  "bank_check",      "split_overlap", "front_sleep", "submit_budget",
    "fragment_barrier", "fence_flush",   "frames_ahead", "pipeline_retention",
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_OPT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

inline bool On(Opt opt) {
    static const unsigned off = [] {
        unsigned mask = 0;
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
