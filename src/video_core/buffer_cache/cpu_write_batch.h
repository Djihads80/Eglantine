// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "common/range_sets.inc"
#include "video_core/buffer_cache/write_preserve_downloads.h"

namespace VideoCommon {

using CpuWriteRange = std::pair<DAddr, std::size_t>;

inline bool ValidCpuWriteRange(DAddr address, u64 size) {
    return address != 0 && size != 0 && address < WritePreserveDownloadHints::AddressLimit &&
           size <= WritePreserveDownloadHints::AddressLimit - address;
}

/// All writes in this snapshot already happened. Remove every CPU-owned byte before a download
/// of a shared page can overwrite a later write in the same batch. Keep unrelated GPU bytes dirty.
template <typename DiscardPending>
Common::RangeSet<DAddr> PrepareCpuWriteBatch(std::span<const CpuWriteRange> writes,
                                           Common::RangeSet<DAddr>& gpu_written,
                                           WritePreserveDownloadHints& hints,
                                           DiscardPending&& discard_pending) {
    for (const auto& [address, size] : writes) {
        if (ValidCpuWriteRange(address, size)) {
            // A pending snapshot may outlive the GPU dirty mark removed by an earlier pop.
            discard_pending(address, size);
            gpu_written.Subtract(address, size);
        }
    }

    Common::RangeSet<DAddr> preserve_pages;
    constexpr u64 page_mask = WritePreserveDownloadHints::PageSize - 1;
    for (const auto& [address, size] : writes) {
        if (!ValidCpuWriteRange(address, size)) {
            continue;
        }
        const DAddr begin = address & ~page_mask;
        const DAddr end = (address + size + page_mask) & ~page_mask;
        if (gpu_written.Intersects(begin, end - begin)) {
            hints.LearnFromWrite(address, size, gpu_written);
            preserve_pages.Add(begin, end - begin);
        }
    }
    return preserve_pages;
}

} // namespace VideoCommon
