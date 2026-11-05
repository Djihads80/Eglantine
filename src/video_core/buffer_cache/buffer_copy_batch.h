// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// Batch consecutive copies from the same source buffer. Preserve every region and its order;
/// the callback must consume the span before returning. Sources that recur later stay separate.
/// Destination regions must not overlap, as in the packed staging allocation for downloads.
template <typename BufferId, typename Func>
size_t ForEachBufferCopyBatch(std::span<const std::pair<BufferCopy, BufferId>> entries, Func&& func) {
    boost::container::small_vector<BufferCopy, 16> copies;
    size_t batches = 0;
    for (size_t index = 0; index < entries.size();) {
        const BufferId source = entries[index].second;
        copies.clear();
        do {
            copies.push_back(entries[index++].first);
        } while (index < entries.size() && entries[index].second == source);
        func(source, std::span<const BufferCopy>{copies.data(), copies.size()});
        ++batches;
    }
    return batches;
}

} // namespace VideoCommon
