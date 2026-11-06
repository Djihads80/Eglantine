// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/unbacked_image_policy.h"

namespace {
using Vulkan::ShouldLeaveImageUnbacked;
constexpr u64 GiB = u64{1} << 30;
constexpr u64 Vram = 16 * GiB;

TEST_CASE("Images up to the texel threshold are always backed", "[video_core]") {
    REQUIRE_FALSE(ShouldLeaveImageUnbacked(u64{1} << 28, 100 * GiB, Vram));
    REQUIRE_FALSE(ShouldLeaveImageUnbacked(12288ULL * 12288, 100 * GiB, Vram));
}

TEST_CASE("A large depth map that fits in a quarter of VRAM is backed", "[video_core]") {
    // 18432x18432 D16: 339M texels, 648 MiB.
    constexpr u64 texels = 18432ULL * 18432;
    REQUIRE_FALSE(ShouldLeaveImageUnbacked(texels, texels * 2, Vram));
    // The same image on a card with 2 GiB of VRAM is too big.
    REQUIRE(ShouldLeaveImageUnbacked(texels, texels * 2, 2 * GiB));
}

TEST_CASE("A sparse texture array far beyond VRAM stays unbacked", "[video_core]") {
    constexpr u64 texels = 4500ULL * 1000 * 1000;
    REQUIRE(ShouldLeaveImageUnbacked(texels, 23 * GiB, Vram));
}

TEST_CASE("The limit is a quarter of the device-local memory", "[video_core]") {
    constexpr u64 texels = (u64{1} << 28) + 1;
    REQUIRE_FALSE(ShouldLeaveImageUnbacked(texels, Vram / 4, Vram));
    REQUIRE(ShouldLeaveImageUnbacked(texels, Vram / 4 + 1, Vram));
}

TEST_CASE("An unknown memory requirement is treated as unbackable", "[video_core]") {
    REQUIRE(ShouldLeaveImageUnbacked((u64{1} << 28) + 1, 0, Vram));
}
} // namespace
