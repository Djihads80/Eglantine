// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>

#include "common/common_types.h"

namespace VideoCommon::Nx2DrawLog {

// EDEN_NX2_DRAW_LOG=1: log draws of programs whose stage buffer writes (ISBEWR) were dropped,
// draws skipped for lack of a pipeline, and a summary every 20000 draws, to compare Fast and
// Accurate runs.
inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_DRAW_LOG") != nullptr;
    return enabled;
}

struct Counters {
    std::atomic<u64> draws{};
    std::atomic<u64> no_pipeline{};
    std::atomic<u64> rejected_stage{};  // no_pipeline: a bound program was rejected
    std::atomic<u64> async_pending{};   // no_pipeline: pipeline still building (async shaders)
    std::atomic<u64> configure_failed{};
    std::atomic<u64> stage_buffer_draws{};
    std::atomic<u64> instanced_draws{};
    std::atomic<u64> base_instance_draws{};
    std::atomic<u64> macro_calls{};
    std::atomic<u64> dirty_macro_calls{};
    std::atomic<u64> dirty_macro_stale{};
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

// Instance count of the draw being recorded (PrepareDraw does not see it).
inline u32& LastInstanceCount() {
    static thread_local u32 count{};
    return count;
}

inline std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::set<u64>& DroppedPrograms() {
    static std::set<u64> programs;
    return programs;
}

inline void NoteDroppedProgram(u64 program_base) {
    std::scoped_lock lock{Mutex()};
    DroppedPrograms().insert(program_base);
}

inline bool IsDroppedProgram(u64 program_base) {
    std::scoped_lock lock{Mutex()};
    return DroppedPrograms().contains(program_base);
}

} // namespace VideoCommon::Nx2DrawLog
