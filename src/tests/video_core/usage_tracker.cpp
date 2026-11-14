// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/usage_tracker.h"

TEST_CASE("Usage tracker resets separated sparse ranges and forgets the old extent",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker(256ULL << 20);
    for (u32 iteration = 0; iteration < 4; ++iteration) {
        const u64 offset = (iteration & 1) != 0 ? 64 : 128ULL << 20;
        tracker.Track(offset, 128);
        tracker.Track(offset + 0x9000, 0x2000);
        REQUIRE(tracker.IsUsed(offset, 128));
        REQUIRE(tracker.IsUsed(offset + 0xa000, 64));
        tracker.Reset();
        CHECK_FALSE(tracker.IsUsed(offset, 128));
        CHECK(std::ranges::all_of(tracker.Pages(), [](u64 word) { return word == 0; }));
        tracker.Reset();
        CHECK(std::ranges::all_of(tracker.Pages(), [](u64 word) { return word == 0; }));
    }
}
