// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/texture_cache/image_info.h"
#include "video_core/textures/decoders.h"

TEST_CASE("Block-linear textures preserve both GOB sector orders", "[texture_layout]") {
    // Each entry identifies a 16-byte sector in one 64-by-8-byte GOB.
    constexpr std::array<u8, 32> sectors{
        0, 2,  16, 18, 1, 3,  17, 19, 4,  6,  20, 22, 5,  7,  21, 23,
        8, 10, 24, 26, 9, 11, 25, 27, 12, 14, 28, 30, 13, 15, 29, 31,
    };
    constexpr std::array<u8, 32> native_sectors{
        0, 4,  16, 20, 1, 5,  17, 21, 2,  6,  18, 22, 3,  7,  19, 23,
        8, 12, 24, 28, 9, 13, 25, 29, 10, 14, 26, 30, 11, 15, 27, 31,
    };
    std::array<u8, 512> tiled{}, expected{};
    for (const bool native_tiling : {false, true}) {
        CAPTURE(native_tiling);
        for (size_t sector = 0; sector < sectors.size(); ++sector) {
            std::fill_n(tiled.begin() + sector * 16, 16, static_cast<u8>(sector));
            std::fill_n(expected.begin() + sector * 16, 16,
                        (native_tiling ? native_sectors : sectors)[sector]);
        }
        for (const u32 bytes_per_pixel : {1U, 2U, 4U, 8U, 16U}) {
            CAPTURE(bytes_per_pixel);
            const u32 width = 64 / bytes_per_pixel;
            std::array<u8, 512> linear{}, encoded{};
            Tegra::Texture::UnswizzleTexture(linear, tiled, bytes_per_pixel, width, 8, 1, 0, 0, 1,
                                             native_tiling);
            REQUIRE(linear == expected);
            Tegra::Texture::SwizzleTexture(encoded, expected, bytes_per_pixel, width, 8, 1, 0, 0, 1,
                                           native_tiling);
            REQUIRE(encoded == tiled);
            linear.fill(0);
            encoded.fill(0);
            Tegra::Texture::UnswizzleSubrect(linear, tiled, bytes_per_pixel, width, 8, 1, 0, 0,
                                             width, 8, 0, 0, 64, native_tiling);
            REQUIRE(linear == expected);
            Tegra::Texture::SwizzleSubrect(encoded, expected, bytes_per_pixel, width, 8, 1, 0, 0,
                                           width, 8, 0, 0, 64, native_tiling);
            REQUIRE(encoded == tiled);
        }
    }
}

TEST_CASE("Native texture descriptors select their sector layout", "[texture_layout]") {
    Tegra::Texture::TICEntry config{};
    config.raw = {0x216af00078d7ffa1ULL, 0x0007002000600081ULL, 0x80000437e880077fULL,
                  0x0000000003000000ULL};
    for (const bool legacy : {false, true}) {
        CAPTURE(legacy);
        config.load_store_hint.Assign(legacy);
        const VideoCommon::ImageInfo info{config};
        REQUIRE(info.UsesNativeSectorLayout(true) == !legacy);
        REQUIRE_FALSE(info.UsesNativeSectorLayout(false));
    }
}
