// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/engines/native_upscale.h"

TEST_CASE("Native upscale uses active regions inside larger texture allocations", "[nx2_video]") {
    const std::array<u32, 12> parameters{128, 40, 23, 1, 0, 0, 959, 539, 0, 0, 1919, 1079};
    const auto regions = Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 40, 23, 1}, {960, 540}, {3840, 2160});
    REQUIRE(regions);
    CHECK(regions->source == std::array<u32, 4>{0, 0, 960, 540});
    CHECK(regions->destination == std::array<u32, 4>{0, 0, 1920, 1080});

    auto offset = parameters;
    offset[4] = 16;
    offset[5] = 8;
    offset[8] = 32;
    offset[9] = 16;
    const auto subregion = Tegra::Engines::DecodeNativeUpscaleRegions(
        offset, {128, 40, 23, 1}, {960, 540}, {3840, 2160});
    REQUIRE(subregion);
    CHECK(subregion->source == std::array<u32, 4>{16, 8, 960, 540});
    CHECK(subregion->destination == std::array<u32, 4>{32, 16, 1920, 1080});
}

TEST_CASE("Native upscale rejects incompatible or invalid region parameters", "[nx2_video]") {
    const std::array<u32, 12> parameters{128, 40, 23, 1, 0, 0, 959, 539, 0, 0, 1919, 1079};
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 41, 23, 1}, {960, 540}, {3840, 2160}));
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        std::span{parameters}.first(11), {128, 40, 23, 1}, {960, 540}, {3840, 2160}));
    for (const std::pair<std::size_t, u32> invalid : {
             std::pair<std::size_t, u32>{6, 960}, {7, 540}, {10, 3840}, {11, 2160},
             {10, 0xffffffff}, {4, 960}, {8, 1920}, {10, 100}, {11, 100}, {5, 540}, {9, 1080}}) {
        auto malformed = parameters;
        malformed[invalid.first] = invalid.second;
        CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
            malformed, {128, 40, 23, 1}, {960, 540}, {3840, 2160}));
    }
}

TEST_CASE("Native upscale handles single pixels and rejects empty allocations", "[nx2_video]") {
    const std::array<u32, 12> parameters{128, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto regions = Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 1, 1, 1}, {1, 1}, {1, 1});
    REQUIRE(regions);
    CHECK(regions->source == std::array<u32, 4>{0, 0, 1, 1});
    CHECK(regions->destination == regions->source);
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 1, 1, 1}, {0, 1}, {1, 1}));
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 1, 1, 1}, {1, 1}, {1, 0}));
}
