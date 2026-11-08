// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/stale_pages.h"

namespace {

using VideoCommon::StalePages;
using Runs = std::vector<std::pair<u64, u64>>;

constexpr u64 Page = StalePages::PAGE_SIZE;

struct Recorder {
    Runs runs;
    void operator()(u64 addr, u64 size) {
        runs.emplace_back(addr, size);
    }
};

} // namespace

TEST_CASE("StalePages marks only whole pages", "[video_core][texture_cache]") {
    StalePages stale;
    Recorder marked;
    stale.Mark(Page + Page / 2, Page / 2, marked);
    REQUIRE(stale.Count() == 0);
    REQUIRE(marked.runs.empty());

    stale.Mark(Page + Page / 2, 3 * Page, marked);
    REQUIRE(stale.Count() == 2);
    REQUIRE(marked.runs == Runs{{2 * Page, 2 * Page}});
    REQUIRE(stale.IsAll(2 * Page, 2 * Page));
    REQUIRE(stale.IsAll(2 * Page + 100, 10));
    REQUIRE_FALSE(stale.IsAll(Page, 2 * Page));
    REQUIRE_FALSE(stale.IsAll(4 * Page - 1, 2));
    REQUIRE_FALSE(stale.IsAll(2 * Page, 0));
}

TEST_CASE("StalePages reports only newly marked runs", "[video_core][texture_cache]") {
    StalePages stale;
    Recorder marked;
    stale.Mark(2 * Page, 2 * Page, marked);
    marked.runs.clear();
    stale.Mark(0, 7 * Page, marked);
    REQUIRE(stale.Count() == 7);
    REQUIRE(marked.runs == Runs{{0, 2 * Page}, {4 * Page, 3 * Page}});
    REQUIRE(stale.IsAll(0, 7 * Page));
}

TEST_CASE("StalePages clears every page a range touches", "[video_core][texture_cache]") {
    StalePages stale;
    Recorder ignored;
    stale.Mark(0, 8 * Page, ignored);

    Recorder cleared;
    stale.Clear(3 * Page - 1, 2, cleared);
    REQUIRE(stale.Count() == 6);
    REQUIRE(cleared.runs == Runs{{2 * Page, 2 * Page}});
    REQUIRE_FALSE(stale.IsAll(0, 8 * Page));
    REQUIRE(stale.IsAll(0, 2 * Page));
    REQUIRE(stale.IsAll(4 * Page, 4 * Page));

    cleared.runs.clear();
    stale.Clear(0, 8 * Page, cleared);
    REQUIRE(stale.Count() == 0);
    REQUIRE(cleared.runs == Runs{{0, 2 * Page}, {4 * Page, 4 * Page}});
    REQUIRE_FALSE(stale.IsAll(0, Page));

    cleared.runs.clear();
    stale.Clear(0, 8 * Page, cleared);
    REQUIRE(cleared.runs.empty());
}

TEST_CASE("StalePages spans bitmap words", "[video_core][texture_cache]") {
    StalePages stale;
    u64 tracked = 0;
    const auto track = [&](u64, u64 size) { tracked += size; };
    stale.Mark(0, 200 * Page, track);
    REQUIRE(stale.Count() == 200);
    REQUIRE(tracked == 200 * Page);
    REQUIRE(stale.IsAll(0, 200 * Page));
    REQUIRE(stale.IsAll(64 * Page, 64 * Page));
    REQUIRE_FALSE(stale.IsAll(0, 201 * Page));

    const auto untrack = [&](u64, u64 size) { tracked -= size; };
    stale.Clear(100 * Page, 1, untrack);
    REQUIRE(stale.Count() == 199);
    REQUIRE(tracked == 199 * Page);
    REQUIRE_FALSE(stale.IsAll(64 * Page, 64 * Page));
    REQUIRE(stale.IsAll(0, 100 * Page));
    REQUIRE(stale.IsAll(101 * Page, 99 * Page));

    stale.Clear(0, 300 * Page, untrack);
    REQUIRE(stale.Count() == 0);
    REQUIRE(tracked == 0);
}

TEST_CASE("StalePages ignores addresses past its bound", "[video_core][texture_cache]") {
    StalePages stale;
    Recorder marked;
    stale.Mark(u64{1} << 40, 4 * Page, marked);
    REQUIRE(stale.Count() == 0);
    REQUIRE(marked.runs.empty());
    REQUIRE_FALSE(stale.IsAll(u64{1} << 40, Page));
}
