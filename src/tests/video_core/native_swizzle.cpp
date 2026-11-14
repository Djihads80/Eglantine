// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/textures/decoders.h"

namespace {

struct Shape {
    u32 width;
    u32 height;
    u32 depth;
    u32 block_height;
    u32 block_depth;
    u32 stride_alignment;
};

u32 Align(u32 value, u32 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Scalar byte-address oracle: no incremental deposits or decoder helpers.
u32 TiledOffset(u32 x_byte, u32 y, u32 z, u32 stride, const Shape& shape, bool native) {
    const u32 block_height = 1U << shape.block_height;
    const u32 block_depth = 1U << shape.block_depth;
    const u32 block_size = (stride + 63) / 64 * 512 * block_height * block_depth;
    const u32 slice_size = (shape.height + 8 * block_height - 1) / (8 * block_height) * block_size;
    u32 address = z / block_depth * slice_size + z % block_depth * 512 * block_height;
    address += y / (8 * block_height) * block_size + y / 8 % block_height * 512;
    address += x_byte / 64 * 512 * block_height * block_depth;
    address += (x_byte & 32) * 8 + (y & 6) * 32 + (x_byte & 16) * 2 +
               (y & 1) * 16 + (x_byte & 15);
    if (native) {
        const u32 bit5 = (address >> 5) & 1;
        const u32 bit6 = (address >> 6) & 1;
        address = (address & ~u32{96}) | (bit5 << 6) | (bit6 << 5);
    }
    return address;
}

} // namespace

TEST_CASE("Full texture swizzles match a scalar byte-address oracle",
          "[video_core][native_swizzle]") {
    constexpr std::array shapes{
        Shape{1, 1, 1, 0, 0, 0}, Shape{3, 7, 1, 0, 0, 0},
        Shape{5, 9, 1, 1, 0, 4}, Shape{17, 17, 1, 3, 0, 0},
        Shape{31, 8, 3, 0, 1, 4}, Shape{32, 15, 3, 1, 1, 0},
        Shape{63, 19, 3, 2, 1, 6}, Shape{64, 33, 1, 2, 0, 6},
        Shape{65, 31, 5, 1, 2, 0}, Shape{129, 9, 2, 3, 1, 4},
    };
    std::mt19937 random{0x5a1221e};
    for (const auto shape : shapes) {
        for (const u32 bpp : {1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U}) {
            for (const bool native : {false, true}) {
                CAPTURE(shape.width, shape.height, shape.depth, bpp, native);
                const u32 pitch = shape.width * bpp;
                const u32 stride = Align(shape.width, 1U << shape.stride_alignment) * bpp;
                const u32 tiled_size = (stride + 63) / 64 * 512 *
                                       Align(shape.height, 8U << shape.block_height) / 8 *
                                       Align(shape.depth, 1U << shape.block_depth);
                std::vector<u8> linear(pitch * shape.height * shape.depth);
                std::generate(linear.begin(), linear.end(), [&] { return static_cast<u8>(random()); });
                std::vector<u8> expected(tiled_size + 64, 0xa5);
                for (u32 z = 0; z < shape.depth; ++z) {
                    for (u32 y = 0; y < shape.height; ++y) {
                        for (u32 x = 0; x < pitch; ++x) {
                            const u32 address = TiledOffset(x, y, z, stride, shape, native);
                            REQUIRE(address < tiled_size);
                            expected[address] = linear[(z * shape.height + y) * pitch + x];
                        }
                    }
                }
                std::vector<u8> tiled(expected.size(), 0xa5);
                Tegra::Texture::SwizzleTexture(tiled, linear, bpp, shape.width, shape.height,
                                               shape.depth, shape.block_height, shape.block_depth,
                                               shape.stride_alignment, native);
                CHECK(tiled == expected);
                std::vector<u8> decoded(linear.size() + 64, 0x5a);
                Tegra::Texture::UnswizzleTexture(decoded, expected, bpp, shape.width, shape.height,
                                                 shape.depth, shape.block_height, shape.block_depth,
                                                 shape.stride_alignment, native);
                CHECK(std::equal(linear.begin(), linear.end(), decoded.begin()));
                CHECK(std::all_of(decoded.end() - 64, decoded.end(), [](u8 byte) { return byte == 0x5a; }));
            }
        }
    }
}

TEST_CASE("Native full texture unswizzle throughput", "[.native_swizzle_bench]") {
    constexpr std::array shapes{Shape{1280, 720, 1, 4, 0, 0},
                                 Shape{256, 256, 1, 3, 0, 0},
                                 Shape{128, 64, 16, 2, 2, 0}};
    for (const auto shape : shapes) {
        constexpr u32 bpp = 4;
        constexpr u32 iterations = 8192;
        const auto bytes = Tegra::Texture::CalculateSize(true, bpp, shape.width, shape.height,
                                                        shape.depth, shape.block_height, shape.block_depth);
        std::vector<u8> tiled(bytes, 0x7b);
        std::vector<u8> linear(shape.width * shape.height * shape.depth * bpp);
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < iterations; ++i) {
            Tegra::Texture::UnswizzleTexture(linear, tiled, bpp, shape.width, shape.height, shape.depth,
                                             shape.block_height, shape.block_depth, 0, true);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const char* flag = std::getenv("EDEN_NX2_NATIVE_SWIZZLE_MASKS");
        std::printf("native_swizzle_bench flag=%s width=%u height=%u depth=%u seconds=%.6f MiB_s=%.2f\n",
                    flag ? flag : "unset", shape.width, shape.height, shape.depth, seconds,
                    static_cast<double>(linear.size()) * iterations / (1024 * 1024) / seconds);
        REQUIRE(std::all_of(linear.begin(), linear.end(), [](u8 byte) { return byte == 0x7b; }));
    }
}
