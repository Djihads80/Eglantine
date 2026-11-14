// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

namespace {
using VideoCommon::UsageTracker;

TEST_CASE("UsageTracker covers partly used 64-byte units", "[video_core][usage_tracker]") {
    UsageTracker tracker(1 << 20);
    tracker.Track(0, 100);
    REQUIRE(tracker.IsUsed(0, 1));
    REQUIRE(tracker.IsUsed(64, 16));
    REQUIRE(tracker.IsUsed(96, 4));
    REQUIRE_FALSE(tracker.IsUsed(128, 64));

    tracker.Reset();
    tracker.Track(60, 8);
    REQUIRE(tracker.IsUsed(0, 4));
    REQUIRE(tracker.IsUsed(64, 4));
    REQUIRE_FALSE(tracker.IsUsed(128, 4));
}

TEST_CASE("UsageTracker does not mark a whole page for small ranges", "[video_core][usage_tracker]") {
    UsageTracker tracker(1 << 20);
    tracker.Track(4096 + 128, 16);
    REQUIRE(tracker.IsUsed(4096 + 128, 16));
    REQUIRE_FALSE(tracker.IsUsed(4096 + 256, 64));
    REQUIRE_FALSE(tracker.IsUsed(4096, 64));

    // A range ending on a page boundary marks nothing in the next page.
    tracker.Reset();
    tracker.Track(4096 - 64, 64);
    REQUIRE(tracker.IsUsed(4096 - 64, 64));
    REQUIRE_FALSE(tracker.IsUsed(4096, 64));
    REQUIRE_FALSE(tracker.IsUsed(4096 + 2048, 64));
}

TEST_CASE("UsageTracker spans pages", "[video_core][usage_tracker]") {
    UsageTracker tracker(1 << 20);
    tracker.Track(4096 - 32, 4096 + 64);
    REQUIRE(tracker.IsUsed(4096 - 32, 1));
    REQUIRE(tracker.IsUsed(4096 + 1024, 1));
    REQUIRE(tracker.IsUsed(2 * 4096, 16));
    REQUIRE_FALSE(tracker.IsUsed(2 * 4096 + 64, 64));
    REQUIRE_FALSE(tracker.IsUsed(0, 64));
}
} // namespace
