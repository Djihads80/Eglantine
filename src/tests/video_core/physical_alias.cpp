// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/physical_alias.h"

TEST_CASE("Framebuffer aliases require matching physical backing on every page", "[physical_alias]") {
    std::array<PAddr, 8> pages{0x10000, 0x22000, 0x33000, 0, 0x10000, 0x22000, 0x33000, 0};
    const auto translate = [&](DAddr address) -> PAddr {
        const u64 page = address / Core::DEVICE_PAGESIZE;
        if (page >= pages.size() || pages[page] == 0) {
            return 0;
        }
        return pages[page] + (address & Core::DEVICE_PAGEMASK);
    };
    CHECK(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0x3000, translate));
    CHECK(VideoCommon::IsSamePhysicalRange(0x123, 0x4123, 0x2edd, translate));
    CHECK(VideoCommon::IsSamePhysicalRange(0x123, 0x4123, 0x2abc, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0x123, 0x4123, 0x2ede, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0x3001, translate));
    pages[6] = 0x44000;
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0x123, 0x4123, 0x2abc, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0x3000, translate));
    CHECK(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0x2000, translate));
    pages[5] = 0;
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0x2000, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0x3000, 0x7000, 1, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0, 0x4001, 1, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(0, 0x4000, 0, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(
        std::numeric_limits<u64>::max() - 0xfff, 0x4000, 0x1000, translate));
    CHECK_FALSE(VideoCommon::IsSamePhysicalRange(
        0, std::numeric_limits<u64>::max() - 0xfff, 0x1000, translate));
}
