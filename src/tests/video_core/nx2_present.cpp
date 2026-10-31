// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present.h"

TEST_CASE("Nx2 presentation crops preserve the native allocation extent", "[nx2_video]") {
    const auto crop = Service::Nx2PresentCrop(0, 1279 | (719 << 16), 2, 3840, 2160);
    CHECK(crop.left == 0);
    CHECK(crop.top == 0);
    CHECK(crop.right == 1280);
    CHECK(crop.bottom == 720);
    const auto disabled = Service::Nx2PresentCrop(0, 1279 | (719 << 16), 1, 3840, 2160);
    CHECK(disabled.right == 3840);
    CHECK(disabled.bottom == 2160);
}

TEST_CASE("Nx2 presentation crops decode inclusive coordinates and clip to the image", "[nx2_video]") {
    const auto crop = Service::Nx2PresentCrop(16 | (32 << 16), 4095 | (4095 << 16), 2, 3840, 2160);
    CHECK(crop.left == 16);
    CHECK(crop.top == 32);
    CHECK(crop.right == 3840);
    CHECK(crop.bottom == 2160);
    const auto pixel = Service::Nx2PresentCrop(16 | (32 << 16), 16 | (32 << 16), 2, 3840, 2160);
    CHECK(pixel.GetWidth() == 1);
    CHECK(pixel.GetHeight() == 1);
    const auto maximum = Service::Nx2PresentCrop(0, 0xffffffff, 2, 65536, 65536);
    CHECK(maximum.right == 65536);
    CHECK(maximum.bottom == 65536);
}

TEST_CASE("Nx2 presentation ignores empty or reversed crop metadata", "[nx2_video]") {
    for (const u32 origin : {3840U, 2160U << 16, 32U | (32U << 16)}) {
        const auto crop = Service::Nx2PresentCrop(origin, 15 | (15 << 16), 2, 3840, 2160);
        CHECK(crop.left == 0);
        CHECK(crop.top == 0);
        CHECK(crop.right == 3840);
        CHECK(crop.bottom == 2160);
    }
}
