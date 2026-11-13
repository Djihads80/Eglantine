// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/nx2_list_ranges.h"

using VideoCommon::Nx2ListRanges;

TEST_CASE("List ranges before the first flush count every byte as GPU-written",
          "[video_core][nx2_list_ranges]") {
    Nx2ListRanges ranges;
    CHECK(ranges.Wrote(0, 4));
    ranges.Note(0x100, 0x200);
    CHECK(ranges.Wrote(0x800, 0x900));
}

TEST_CASE("List ranges tell lists apart from GPU writes on the same page",
          "[video_core][nx2_list_ranges]") {
    Nx2ListRanges ranges;
    ranges.Reset();
    CHECK_FALSE(ranges.Wrote(0, 0x1000));
    ranges.Note(0x100, 0x200);
    CHECK(ranges.Wrote(0x180, 0x190));
    CHECK(ranges.Wrote(0x0f0, 0x101));
    CHECK(ranges.Wrote(0x1ff, 0x300));
    CHECK_FALSE(ranges.Wrote(0x0c0, 0x100)); // ends where the write begins
    CHECK_FALSE(ranges.Wrote(0x200, 0x2c8)); // begins where the write ends
    ranges.Reset();
    CHECK_FALSE(ranges.Wrote(0x180, 0x190));
}

TEST_CASE("List ranges join touching writes and bound too many", "[video_core][nx2_list_ranges]") {
    Nx2ListRanges ranges;
    ranges.Reset();
    ranges.Note(0x000, 0x010);
    ranges.Note(0x010, 0x020);
    REQUIRE(ranges.ranges.size() == 1);
    CHECK(ranges.ranges[0] == std::pair<u32, u32>{0x000, 0x020});
    ranges.Note(0x30, 0x30); // empty
    CHECK(ranges.ranges.size() == 1);

    for (u32 i = 1; i < Nx2ListRanges::MaxRanges; ++i) {
        ranges.Note(i * 0x100, i * 0x100 + 8);
    }
    CHECK(ranges.ranges.size() == Nx2ListRanges::MaxRanges);
    CHECK_FALSE(ranges.Wrote(0x0c0, 0x0f0));
    ranges.Note(0xf00, 0xf10);
    REQUIRE(ranges.ranges.size() == 1);
    CHECK(ranges.ranges[0] == std::pair<u32, u32>{0x000, 0xf10});
    // Bounded: the gaps count as written now.
    CHECK(ranges.Wrote(0x0c0, 0x0f0));
    CHECK_FALSE(ranges.Wrote(0xf10, 0x1000));
}
