// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_FLIGHT=1: counts rare and common events per presented frame (one record a frame in
// nx2_flight.bin in the log directory) and draws the frame's number as 16 black or white squares
// in the bottom left corner of the picture, so a frame of a screen recording can be matched to
// its record. For glitches that last a frame: what was different in that frame.
namespace VideoCommon::Nx2Flight {

enum Counter : u32 {
    Draws,
    MeshDraws,
    Dispatches,
    Clears,
    PassEnds,
    Flushes,
    Uploads,
    OrderedUploads,
    InlineUploads,
    BufferDownloads,
    BuffersCreated,
    BuffersDeleted,
    SyncSkipClean,
    SyncSkipCached,
    SyncWalks,
    UboFast,
    UboCached,
    UboZero,
    ImagesCreated,
    ImagesDeleted,
    ImageUploads,
    StaleSkips,
    AliasCopies,
    ListsPredicted,
    ListsWrong,
    ListWaits,
    GuestFlushes,
    GuestFlushWaits,
    SyncOps,
    ChannelSwitches,
    PipelineLookups,
    FeedbackEnds,
    DescriptorReuses,
    DescriptorWrites,
    MeshChunkReuses,
    MeshChunkNew,
    Count = 48,
};

inline std::atomic<bool> g_enabled{};
inline std::array<std::atomic<u32>, Count> g_counters{};
/// The number of the frame being presented (its record's).
inline std::atomic<u32> g_frame{};

/// The frame's draws and dispatches in order (the recording thread only): one word each, the
/// pipeline's id in bits 0-31, a count (vertices or task workgroups, or the dispatch's width) in
/// bits 32-51, flags in bits 52-55 and the channel in bits 56-63.
inline std::vector<u64> g_sequence;
inline constexpr u64 StepIndexed = 1, StepMesh = 2, StepEmpty = 4, StepCompute = 8;

inline void Step(u64 pipeline_id, u64 count, u64 flags, u64 channel) {
    if (g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
        g_sequence.push_back((pipeline_id & 0xffffffffULL) | ((std::min)(count, u64{0xfffff}) << 32) |
                             ((flags & 0xf) << 52) | ((channel & 0xff) << 56));
    }
}

inline void Note(Counter counter, u32 amount = 1) {
    if (g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
        g_counters[counter].fetch_add(amount, std::memory_order_relaxed);
    }
}

} // namespace VideoCommon::Nx2Flight
