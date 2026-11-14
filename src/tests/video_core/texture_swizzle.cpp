// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The block-linear swizzle copies whole 16-byte runs when the pixel size allows it. This checks
// that against a pixel-at-a-time reference, in both directions and with the native GOB twist.

#include <cstring>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/alignment.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "video_core/textures/decoders.h"

namespace {

using namespace Tegra::Texture;

constexpr u32 OriginX = 0;
constexpr u32 OriginY = 0;
constexpr u32 OriginZ = 0;

constexpr u32 NativeGobOffset(u32 offset, bool native_tiling) {
    if (!native_tiling) {
        return offset;
    }
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}

template <u32 mask>
constexpr u32 Pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit) {
            result |= m & (~m + 1);
        }
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void IncrPdep(u32& value) {
    static constexpr u32 swizzled_incr = Pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

/// The original one-pixel-at-a-time loop: the reference the chunked path must match exactly.
template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void ReferenceImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height,
                   u32 depth, u32 block_height, u32 block_depth, u32 stride,
                   bool native_tiling) {
    const u32 pitch = width * BYTES_PER_PIXEL;
    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;
    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + OriginZ;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        for (u32 line = 0; line < height; ++line) {
            const u32 y = line + OriginY;
            const u32 swizzled_y = Pdep<SWIZZLE_Y_BITS>(y);
            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);
            u32 swizzled_x = Pdep<SWIZZLE_X_BITS>(OriginX * BYTES_PER_PIXEL);
            for (u32 column = 0; column < width;
                 ++column, IncrPdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + OriginX) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;
                const u32 swizzled_offset = NativeGobOffset(
                    offset_z + offset_y + offset_x + (swizzled_x | swizzled_y), native_tiling);
                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;
                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];
                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
}

/// Mirrors the public wrapper: normalise the pixel size, then run the reference loop.
template <bool TO_LINEAR>
void ReferenceSwizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    const u32 normalised_width = (width * bytes_per_pixel) >> new_bpp;
    const u32 normalised_bpp = 1U << new_bpp;
    const auto run = [&](auto bpp_tag) {
        ReferenceImpl<TO_LINEAR, decltype(bpp_tag)::value>(output, input, normalised_width, height,
                                                          depth, block_height, block_depth, stride,
                                                          native_tiling);
    };
    switch (normalised_bpp) {
    case 1:
        run(std::integral_constant<u32, 1>{});
        break;
    case 2:
        run(std::integral_constant<u32, 2>{});
        break;
    case 4:
        run(std::integral_constant<u32, 4>{});
        break;
    case 8:
        run(std::integral_constant<u32, 8>{});
        break;
    default:
        run(std::integral_constant<u32, 16>{});
        break;
    }
}

struct Shape {
    u32 bytes_per_pixel;
    u32 width;
    u32 height;
    u32 depth;
    u32 block_height;
    u32 block_depth;
    u32 stride_alignment;
    bool native_tiling;
};

std::vector<u8> RandomBytes(size_t size, u32 seed) {
    std::mt19937 rng{seed};
    std::vector<u8> bytes(size);
    for (u8& byte : bytes) {
        byte = static_cast<u8>(rng() & 0xff);
    }
    return bytes;
}

void CheckShape(const Shape& shape, u32 seed) {
    const size_t linear = static_cast<size_t>(shape.width) * shape.height * shape.depth *
                          shape.bytes_per_pixel;
    const size_t tiled = CalculateSize(true, shape.bytes_per_pixel, shape.width, shape.height,
                                       shape.depth, shape.block_height, shape.block_depth);
    // CalculateSize does not know about stride_alignment, so a padded row can reach past it, and
    // the extra room also catches a copy that runs one byte over the end.
    constexpr size_t Slack = size_t{1} << 20;
    const std::vector<u8> tiled_in = RandomBytes(tiled + Slack, seed);

    std::vector<u8> got_linear(linear + Slack, 0);
    std::vector<u8> want_linear(linear + Slack, 0);
    UnswizzleTexture(got_linear, tiled_in, shape.bytes_per_pixel, shape.width, shape.height,
                     shape.depth, shape.block_height, shape.block_depth, shape.stride_alignment,
                     shape.native_tiling);
    ReferenceSwizzle<false>(want_linear, tiled_in, shape.bytes_per_pixel, shape.width, shape.height,
                            shape.depth, shape.block_height, shape.block_depth,
                            shape.stride_alignment, shape.native_tiling);
    REQUIRE(memcmp(got_linear.data(), want_linear.data(), want_linear.size()) == 0);

    const std::vector<u8> linear_in = RandomBytes(linear + Slack, seed + 1);
    std::vector<u8> got_tiled(tiled + Slack, 0);
    std::vector<u8> want_tiled(tiled + Slack, 0);
    SwizzleTexture(got_tiled, linear_in, shape.bytes_per_pixel, shape.width, shape.height,
                   shape.depth, shape.block_height, shape.block_depth, shape.stride_alignment,
                   shape.native_tiling);
    ReferenceSwizzle<true>(want_tiled, linear_in, shape.bytes_per_pixel, shape.width, shape.height,
                           shape.depth, shape.block_height, shape.block_depth,
                           shape.stride_alignment, shape.native_tiling);
    REQUIRE(memcmp(got_tiled.data(), want_tiled.data(), want_tiled.size()) == 0);
}

} // Anonymous namespace

TEST_CASE("Texture swizzle matches the per-pixel reference", "[video_core][textures]") {
    u32 seed = 0;
    for (const u32 bpp : {1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U}) {
        for (const u32 width : {16U, 32U, 64U, 100U, 320U}) {
            for (const u32 height : {1U, 8U, 17U}) {
                for (const u32 block_height : {0U, 4U}) {
                    for (const bool native_tiling : {false, true}) {
                        for (const u32 depth : {1U, 2U}) {
                            for (const u32 stride_alignment : {1U, 3U}) {
                                CheckShape({bpp, width, height, depth, block_height, 0,
                                            stride_alignment, native_tiling},
                                           seed++);
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Texture swizzle round-trips", "[video_core][textures]") {
    for (const u32 bpp : {1U, 2U, 4U, 8U}) {
        for (const u32 width : {64U, 1576U}) {
            constexpr u32 height = 24;
            const size_t linear = static_cast<size_t>(width) * height * bpp;
            const size_t tiled = CalculateSize(true, bpp, width, height, 1, 4, 0);
            const std::vector<u8> original = RandomBytes(linear, width + bpp);

            std::vector<u8> tiled_bytes(tiled + GOB_SIZE, 0);
            std::vector<u8> linear_bytes(linear, 0);
            SwizzleTexture(tiled_bytes, original, bpp, width, height, 1, 4, 0, 1, false);
            UnswizzleTexture(linear_bytes, tiled_bytes, bpp, width, height, 1, 4, 0, 1, false);

            REQUIRE(memcmp(linear_bytes.data(), original.data(), linear) == 0);
        }
    }
}
