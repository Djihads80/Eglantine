// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/nx2_video_decoder.h"

using Tegra::Host1x::NvdecCommon::VideoCodec;
using Tegra::Host1x::ParseNx2VideoConfiguration;

TEST_CASE("Nx2 video MIME configurations retain their codec", "[nx2_video]") {
    const std::array cases{
        std::pair{std::string_view{"video/vp9"}, VideoCodec::VP9},
        std::pair{std::string_view{"video/vp8"}, VideoCodec::VP8},
        std::pair{std::string_view{"video/avc"}, VideoCodec::H264},
        std::pair{std::string_view{"video/h264"}, VideoCodec::H264},
    };
    for (const auto& [mime, codec] : cases) {
        std::vector<u8> data(mime.begin(), mime.end());
        for (const bool terminated : {false, true}) {
            if (terminated) {
                data.push_back(0);
            }
            const auto parsed = ParseNx2VideoConfiguration(data);
            REQUIRE(parsed);
            CHECK(parsed->codec == codec);
            CHECK(parsed->extradata.empty());
        }
    }
}

TEST_CASE("Nx2 video binary H264 parameters preserve embedded zeroes", "[nx2_video]") {
    for (const size_t sps_prefix : {3U, 4U}) {
        for (const size_t pps_prefix : {3U, 4U}) {
            std::vector<u8> data(sps_prefix, 0);
            data.back() = 1;
            data.insert(data.end(), {0x67, 0x42, 0, 0x1e, 0x80});
            data.insert(data.end(), pps_prefix, 0);
            data.back() = 1;
            data.insert(data.end(), {0x68, 0x80});
            const auto parsed = ParseNx2VideoConfiguration(data);
            REQUIRE(parsed);
            CHECK(parsed->codec == VideoCodec::H264);
            CHECK(parsed->extradata.data() == data.data());
            CHECK(std::ranges::equal(parsed->extradata, data));
        }
    }
}

TEST_CASE("Nx2 video rejects unknown and incomplete binary configurations", "[nx2_video]") {
    const std::vector<std::vector<u8>> rejected{
        {},
        {'v', 'i', 'd', 'e', 'o', '/', 'u', 'n', 'k', 'n', 'o', 'w', 'n', 0},
        {0, 0, 0, 1},
        {0, 0, 1, 0x67, 0x42, 0, 0x1e, 0x80},
        {0, 0, 1, 0x68, 0x80},
        {0, 0, 1, 0x67, 0x42, 0, 0x1e, 0x80, 0, 0, 1},
        {0, 0, 1, 0x67, 0x42, 0, 0x1e, 0x80, 0, 0, 1, 0x68},
        {0, 0, 1, 0xe7, 0x42, 0, 0x1e, 0x80, 0, 0, 1, 0x68, 0x80},
        {0, 0, 1, 0x65, 0x42, 0, 0x1e, 0x80, 0, 0, 1, 0x68, 0x80},
        {0, 0, 1, 0x67, 0x42, 0, 0x1e, 0x80, 0, 0, 1, 0xe8, 0x80},
    };
    for (const auto& data : rejected) {
        CHECK_FALSE(ParseNx2VideoConfiguration(data));
    }
}
