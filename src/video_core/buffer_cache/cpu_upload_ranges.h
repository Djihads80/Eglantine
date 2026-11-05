// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/common_types.h"
namespace VideoCommon {
// MemoryTracker emits whole dirty pages and consumes their CPU bits. Preserve
// every emitted byte except intervals already owned by newer host-GPU writes.
// Emit receives [begin,end); no clipping to the caller's smaller query is valid.
template <typename Tracker, typename GpuWritten, typename Emit>
void ForEachCpuUploadExcludingGpu(Tracker& tracker, GpuWritten& gpu_written,
                                 DAddr address, u32 size, Emit&& emit) {
    tracker.ForEachUploadRange(address, size, [&](u64 first, u64 bytes) {
        DAddr cursor = first;
        gpu_written.ForEachInRange(first, bytes, [&](DAddr gpu_first, DAddr gpu_end) {
            if (cursor != gpu_first) emit(cursor, gpu_first);
            cursor = gpu_end;
        });
        if (cursor != first + bytes) emit(cursor, first + bytes);
    });
}
} // namespace VideoCommon
