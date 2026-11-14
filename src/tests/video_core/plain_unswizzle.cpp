// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/plain_unswizzle.h"
#include "video_core/textures/decoders.h"

namespace {
using namespace VideoCommon;

ImageInfo Plain() {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.size = {960, 540, 1};
    info.resources = {.levels = 1, .layers = 1};
    info.block = {0, 3, 0};
    info.native_tiling = true;
    return info;
}

std::optional<u32> ShaderSourceWord(const PlainUnswizzleParameters& p, u32 index) {
    if (index >= p.word_count) {
        return std::nullopt;
    }
    const u32 x = (index % p.words_per_row) * 4, y = index / p.words_per_row;
    u32 gob = ((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) |
              ((y & 1) << 4) | (x & 15);
    if (p.native_tiling) {
        const u32 difference = ((gob >> 5) ^ (gob >> 6)) & 1;
        gob ^= (difference << 5) | (difference << 6);
    }
    const u32 block_y = y >> 3;
    return ((block_y >> p.block_height) * p.block_size +
            ((block_y & p.block_height_mask) << 9) + ((x >> 6) << p.x_shift) + gob) / 4;
}
}

TEST_CASE("Plain GPU unswizzle parameters preserve CPU decoder bytes and padded dispatch bounds",
          "[video_core][plain_unswizzle]") {
    constexpr std::array widths{4U, 17U, 65U, 81U, 97U, 128U, 257U, 960U};
    constexpr std::array heights{1U, 7U, 8U, 9U, 33U, 65U, 97U, 540U};
    for (const auto format : {PixelFormat::A8B8G8R8_UNORM, PixelFormat::R16G16B16A16_FLOAT,
                              PixelFormat::R8_UNORM, PixelFormat::D32_FLOAT}) {
        for (u32 index = 0; index < widths.size(); ++index) {
            auto info = Plain();
            info.format = format;
            info.size = {widths[index], heights[index], 1};
            info.block.height = index % 6;
            info.tile_width_spacing = index % 3;
            for (const bool native : {false, true}) {
                info.native_tiling = native;
                const u32 bpp = VideoCore::Surface::BytesPerBlock(format);
                if ((u64{info.size.width} * bpp) % 4 != 0) {
                    CHECK_FALSE(MakePlainUnswizzleParameters(info));
                    continue;
                }
                const auto params = MakePlainUnswizzleParameters(info);
                REQUIRE(params);
                std::vector<u8> source(size_t{params->source_words} * 4);
                for (size_t i = 0; i < source.size(); ++i) {
                    source[i] = static_cast<u8>((i * 71) ^ (i >> 7) ^ index);
                }
                std::vector<u8> expected(size_t{params->word_count} * 4 + 64, 0xcd);
                auto actual = expected;
                const auto sw = FullUploadSwizzles(info).front();
                Tegra::Texture::UnswizzleTexture(expected, source, bpp, info.size.width,
                                                 info.size.height, 1, sw.block.height, sw.block.depth,
                                                 CalculateLevelStrideAlignment(info, 0), native);
                const u32 padded = Common::AlignUp(params->word_count, 256U);
                for (u32 word = 0; word < padded; ++word) {
                    const auto at = ShaderSourceWord(*params, word);
                    if (word >= params->word_count) {
                        CHECK_FALSE(at);
                        continue;
                    }
                    REQUIRE(at);
                    REQUIRE(*at < params->source_words);
                    std::memcpy(actual.data() + size_t{word} * 4,
                                source.data() + size_t{*at} * 4, 4);
                }
                CHECK(actual == expected);
            }
        }
    }
}

TEST_CASE("Plain GPU unswizzle rejects unsupported shapes and oversized padded footprints",
          "[video_core][plain_unswizzle]") {
    auto info = Plain();
    REQUIRE(MakePlainUnswizzleParameters(info));
    SECTION("Mip chain") { info.resources.levels = 2; }
    SECTION("Array") { info.resources.layers = 2; }
    SECTION("Volume") { info.type = ImageType::e3D; info.size.depth = 2; }
    SECTION("Multisample") { info.num_samples = 4; }
    SECTION("Compressed") { info.format = PixelFormat::BC1_RGBA_UNORM; }
    SECTION("Sparse") { info.is_sparse = true; }
    SECTION("Empty") { info.size.width = 0; }
    SECTION("Unaligned row") { info.format = PixelFormat::R8_UNORM; info.size.width = 81; }
    SECTION("Block depth") { info.block.depth = 1; }
    SECTION("Extreme block height") { info.block.height = 31; }
    SECTION("Extreme tile spacing") { info.tile_width_spacing = 31; }
    SECTION("Overflowing logical extent") { info.size.width = (std::numeric_limits<u32>::max)(); }
    SECTION("Padding beyond cap") { info.size = {1, 8 * 1024 * 1024, 1}; }
    CHECK_FALSE(MakePlainUnswizzleParameters(info));
}
