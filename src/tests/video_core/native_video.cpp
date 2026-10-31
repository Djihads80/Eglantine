// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#include "video_core/host1x/native_video.h"
#include "video_core/textures/decoders.h"

TEST_CASE("Native video rejects unconfigured input", "[native-video]") {
    Tegra::Host1x::NativeVideoDecoder decoder;
    std::vector<Tegra::Host1x::NativeVideoFrame> frames;
    REQUIRE_FALSE(decoder.ConfigureH264({}));
    REQUIRE_FALSE(decoder.Decode({}, 0, false, frames));
    REQUIRE_FALSE(decoder.Decode({}, 0, true, frames));
    REQUIRE(frames.empty());
}

TEST_CASE("Native decoder RGBA tiling round trips", "[native-video]") {
    constexpr u32 width = 32, height = 136, block_height = 4;
    std::vector<u8> linear(width * height * 4);
    for (size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<u8>((i * 13) ^ (i >> 5));
    }
    std::vector<u8> tiled(Tegra::Texture::CalculateSize(true, 4, width, height, 1, block_height, 0));
    std::vector<u8> restored(linear.size());
    Tegra::Texture::SwizzleTexture(tiled, linear, 4, width, height, 1, block_height, 0, 1, true);
    Tegra::Texture::UnswizzleTexture(restored, tiled, 4, width, height, 1, block_height, 0, 1, true);
    REQUIRE(restored == linear);
}

TEST_CASE("Native H264 captured packets decode with timestamps", "[native-video][.]") {
    const char* directory = std::getenv("EDEN_NATIVE_VIDEO_FIXTURE");
    if (!directory) {
        SKIP("Requires owner's local captured input, not distributed with the tests");
    }
    const auto read = [&](const std::string& name) {
        std::ifstream file{std::filesystem::path{directory} / name, std::ios::binary};
        REQUIRE(file.good());
        return std::vector<u8>(std::istreambuf_iterator<char>{file}, {});
    };
    Tegra::Host1x::NativeVideoDecoder decoder;
    REQUIRE(decoder.ConfigureH264(read("video-config.bin")));
    std::vector<Tegra::Host1x::NativeVideoFrame> frames;
    for (u32 i = 0; i < 8; ++i) {
        REQUIRE(decoder.Decode(read("video-packet-" + std::to_string(i) + ".bin"),
                               i * 1001000LL / 30, false, frames));
    }
    REQUIRE(decoder.Decode({}, 0, true, frames));
    REQUIRE(frames.size() == 8);
    REQUIRE_FALSE(frames.empty());
    for (size_t i = 0; i < frames.size(); ++i) {
        REQUIRE(frames[i].width == 3840);
        REQUIRE(frames[i].height == 2160);
        REQUIRE(frames[i].rgba.size() == 3840 * 2160 * 4);
        REQUIRE(frames[i].pts == static_cast<s64>(i * 1001000LL / 30));
    }
}
