// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_decoder_image.h"
#include "video_core/textures/decoders.h"

TEST_CASE("Nx2 RGBA block height follows the native two-dimensional layout", "[nx2_video]") {
    for (const auto [height, expected] : std::array<std::pair<u32, u32>, 12>{
             {{1, 0}, {10, 0}, {11, 1}, {21, 1}, {22, 2}, {42, 2},
              {43, 3}, {85, 3}, {86, 4}, {128, 4}, {2160, 4}, {65536, 4}}}) {
        for (u32 code : {2U, 5U}) {
            std::array<u32, 16> preference{};
            preference[1] = code;
            preference[2] = 3839 | ((height - 1) << 16);
            const auto layout = Service::Nx2CalculateDecoderImageLayout(preference.data());
            CHECK(layout.block_linear);
            CHECK(layout.format == Tegra::Host1x::Nx2PictureFormat::Rgba8);
            CHECK(layout.gob_height_log2 == expected);
            CHECK(layout.pitch == 15360);
        }
    }
}

TEST_CASE("Nx2 tiled RGBA CPU descriptors read the same bytes as native textures", "[nx2_video]") {
    for (const auto [width, height] : std::array<std::pair<u32, u32>, 4>{
             {{1, 1}, {17, 11}, {65, 43}, {3840, 2160}}}) {
        std::array<u32, 16> preference{};
        preference[1] = 2;
        preference[2] = (width - 1) | ((height - 1) << 16);
        const auto layout = Service::Nx2CalculateDecoderImageLayout(preference.data());
        constexpr u32 offset = 0x3000;
        const auto descriptor = Service::Nx2DecoderCpuImageDescriptor(layout, offset);
        REQUIRE(descriptor[0] == 11);
        REQUIRE(descriptor[2] == offset);
        const u32 columns = descriptor[4] & 0xffff;
        const u32 block_width = descriptor[5] & 0xffff;
        const u32 block_height = descriptor[5] >> 16;
        REQUIRE(block_width == 16);
        REQUIRE(block_height == (8U << layout.gob_height_log2));
        REQUIRE(columns * 64 == layout.pitch);
        std::vector<u8> linear(layout.pitch * height);
        std::vector<u8> reconstructed(linear.size());
        std::vector<u8> tiled(offset + layout.size);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                for (u32 c = 0; c < 4; ++c) {
                    linear[y * layout.pitch + x * 4 + c] =
                        static_cast<u8>(x * 13 + y * 29 + c * 47);
                }
            }
        }
        Tegra::Texture::SwizzleSubrect(std::span{tiled}.subspan(offset), linear, 4, width, height,
                                       1, 0, 0, width, height, layout.gob_height_log2, 0,
                                       static_cast<u32>(layout.pitch), true);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u64 block = (y / block_height) * columns + x / block_width;
                const u64 address = descriptor[2] + block * block_width * block_height * 4 +
                                    ((y % block_height) / 8) * 512 + ((x % 16) / 8) * 256 +
                                    ((y % 8) / 4) * 128 + ((x % 8) / 4) * 64 +
                                    (y % 4) * 16 + (x % 4) * 4;
                if (address + 4 > tiled.size()) {
                    FAIL("CPU descriptor addressed beyond its native image allocation");
                }
                std::memcpy(reconstructed.data() + y * layout.pitch + x * 4,
                            tiled.data() + address, 4);
            }
        }
        CHECK(linear == reconstructed);
        if (width == 3840) {
            CHECK(layout.size == 0x1fe0000);
            CHECK(columns == 240);
            CHECK(block_height == 128);
        }
    }
}

TEST_CASE("Nx2 NV12 and RGBA CPU output retains its linear contract", "[nx2_video]") {
    for (u32 code : {1U, 3U, 4U, 6U}) {
        std::array<u32, 16> preference{};
        preference[1] = code;
        preference[2] = 16 | (10 << 16);
        const auto layout = Service::Nx2CalculateDecoderImageLayout(preference.data());
        CHECK_FALSE(layout.block_linear);
        const auto descriptor = Service::Nx2DecoderCpuImageDescriptor(layout, 0x4000);
        const bool nv12 = code == 1 || code == 4;
        CHECK(descriptor[0] == (nv12 ? 10U : 8U));
        CHECK(descriptor[2] == 0x4000);
        CHECK(descriptor[4] == (nv12 ? 64U : 128U));
        if (nv12) {
            CHECK(descriptor[6] == 0x4000 + layout.chroma_offset);
            CHECK(descriptor[8] == layout.pitch);
        }
    }
}
