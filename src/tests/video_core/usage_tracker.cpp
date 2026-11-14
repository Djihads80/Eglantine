// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

TEST_CASE("Usage tracking preserves partial pages, holes and reset across covered spans",
          "[video_core][usage_tracker]") {
    constexpr u64 Page = 4096;
    VideoCommon::UsageTracker tracker(16 * Page);
    tracker.Track(Page + 64, 3 * Page - 128);
    CHECK_FALSE(tracker.IsUsed(Page, 64));
    CHECK_FALSE(tracker.IsUsed(4 * Page - 64, 64));
    CHECK(tracker.IsUsed(2 * Page, Page));
    tracker.Track(7 * Page, 2 * Page);
    CHECK_FALSE(tracker.IsUsed(5 * Page, Page));
    tracker.Track(3 * Page, 5 * Page);
    CHECK(tracker.IsUsed(5 * Page, Page));
    CHECK_FALSE(tracker.IsUsed(Page, 64));
    CHECK_FALSE(tracker.IsUsed(10 * Page, Page));
    tracker.Track(Page, Page);
    tracker.Track(2 * Page + 64, 6 * Page - 128);
    CHECK(tracker.IsUsed(Page, 8 * Page));
    CHECK_FALSE(tracker.IsUsed(9 * Page, 64));
    tracker.Reset();
    CHECK_FALSE(tracker.IsUsed(0, 16 * Page));
    tracker.Track(2 * Page + 64, 6 * Page - 128);
    CHECK(tracker.IsUsed(4 * Page, Page));
    CHECK_FALSE(tracker.IsUsed(Page, Page));
    CHECK_FALSE(tracker.IsUsed(8 * Page, Page));
}

TEST_CASE("Covered-page tracking preserves queries and never loses registered usage",
          "[video_core][usage_tracker]") {
    constexpr u64 Size = 64 * 1024;
    constexpr u64 Granule = 64;
    VideoCommon::UsageTracker cached(Size);
    VideoCommon::UsageTracker control(Size);
    std::vector<bool> used(Size / Granule);
    std::mt19937 random{0x60f05};
    const auto query = [&](u64 offset, u64 size) {
        bool expected = false;
        if (size != 0) {
            for (u64 bit = offset / Granule; bit <= (offset + size - 1) / Granule; ++bit) {
                expected |= used[bit];
            }
        }
        const bool actual = cached.IsUsed(offset, size);
        CHECK(actual == control.IsUsed(offset, size));
        // Existing queries can conservatively include an earlier prefix of the last page.
        // Every byte marked in the independent oracle must nevertheless remain observable.
        CHECK((!expected || actual));
    };
    for (u32 iteration = 0; iteration < 1000; ++iteration) {
        if (iteration % 37 == 0) {
            cached.Reset();
            control.Reset();
            std::fill(used.begin(), used.end(), false);
        }
        const u64 offset = random() % Size;
        const u64 size = random() % (Size - offset + 1);
        cached.Track(offset, size);
        control.Track(offset, size);
        if (size != 0) {
            for (u64 bit = offset / Granule; bit <= (offset + size - 1) / Granule; ++bit) {
                used[bit] = true;
            }
        }
        for (u32 sample = 0; sample < 20; ++sample) {
            const u64 at = random() % Size;
            query(at, random() % (Size - at + 1));
            query(at, 1);
        }
        query(offset, size);
        query(offset, 0);
    }
}
