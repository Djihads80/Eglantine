// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <optional>

#include "common/alignment.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/util.h"

namespace VideoCommon {

struct PlainUnswizzleParameters {
    u32 words_per_row;
    u32 word_count;
    u32 block_size;
    u32 x_shift;
    u32 block_height;
    u32 block_height_mask;
    u32 native_tiling;
    u32 source_words;
};
static_assert(sizeof(PlainUnswizzleParameters) == 32);

inline std::optional<PlainUnswizzleParameters> MakePlainUnswizzleParameters(const ImageInfo& info) {
    constexpr u64 MaxBytes = u64{32} << 20;
    using namespace VideoCore::Surface;
    if (info.type != ImageType::e2D || info.resources.levels != 1 || info.resources.layers != 1 ||
        info.num_samples != 1 || info.is_sparse || info.size.width == 0 || info.size.height == 0 ||
        info.size.depth != 1 || info.block.width != 0 || info.block.depth != 0 ||
        info.block.height > 5 || info.tile_width_spacing > 5 || info.format == PixelFormat::Invalid ||
        DefaultBlockWidth(info.format) != 1 || DefaultBlockHeight(info.format) != 1) {
        return std::nullopt;
    }
    const u32 bpp = BytesPerBlock(info.format);
    const u64 row_bytes = u64{info.size.width} * bpp;
    if (!std::has_single_bit(bpp) || row_bytes == 0 || row_bytes % 4 != 0 ||
        row_bytes > MaxBytes || info.size.height > MaxBytes / row_bytes) {
        return std::nullopt;
    }
    const u32 alignment = CalculateLevelStrideAlignment(info, 0);
    if (alignment > 24) {
        return std::nullopt;
    }
    const u64 stride = Common::AlignUp(u64{info.size.width}, u64{1} << alignment) * bpp;
    const u64 gob_stride = Common::AlignUp(stride, u64{64});
    const u64 aligned_height = Common::AlignUp(u64{info.size.height}, u64{8} << info.block.height);
    if (gob_stride > MaxBytes || aligned_height > MaxBytes / gob_stride) {
        return std::nullopt;
    }
    const auto swizzle = FullUploadSwizzles(info).front();
    const auto layout = Accelerated::MakeBlockLinearSwizzle2DParams(swizzle, info);
    const u64 source_bytes = CalculateGuestSizeInBytes(info);
    const u64 last_block_y = (info.size.height - 1) >> 3;
    const u64 upper_bound = (last_block_y >> layout.block_height) * layout.block_size +
                           ((last_block_y & layout.block_height_mask) << 9) +
                           (((row_bytes - 1) >> 6) << layout.x_shift) + 512;
    if (source_bytes > MaxBytes || source_bytes % 4 != 0 || upper_bound > source_bytes) {
        return std::nullopt;
    }
    return PlainUnswizzleParameters{
        .words_per_row = static_cast<u32>(row_bytes / 4),
        .word_count = static_cast<u32>(row_bytes * info.size.height / 4),
        .block_size = layout.block_size,
        .x_shift = layout.x_shift,
        .block_height = layout.block_height,
        .block_height_mask = layout.block_height_mask,
        .native_tiling = layout.native_tiling,
        .source_words = static_cast<u32>(source_bytes / 4),
    };
}

} // namespace VideoCommon
