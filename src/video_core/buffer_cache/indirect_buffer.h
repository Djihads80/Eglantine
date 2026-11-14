// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/buffer_cache/buffer_cache_base.h"

namespace VideoCommon {
// Dispatch arguments are three GPU-readable words. DiscardWrite would remove the producer's
// pending downloads, allowing a later CPU-dirty page upload to replace them with stale bytes.
// MarkUsage also prevents a later upload from being reordered ahead of this read.
template <class Cache> auto ObtainIndirectBuffer(Cache &cache, GPUVAddr address) {
    auto result = cache.ObtainBuffer(address, 12, ObtainBufferSynchronize::FullSynchronize,
                                     ObtainBufferOperation::DoNothing);
    result.first->MarkUsage(result.second, 12);
    return result;
}
} // namespace VideoCommon
