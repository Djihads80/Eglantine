// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/write_preserve_downloads.h"

namespace {

using VideoCommon::WritePreserveDownloadHints;
using Range = std::pair<DAddr, u64>;
constexpr u64 Page = WritePreserveDownloadHints::PageSize;
constexpr u64 Block = WritePreserveDownloadHints::BlockSize;
constexpr DAddr Base = 16 * Block;

struct DeviceTracker {
    void UpdatePagesCachedCount(DAddr, size_t, s32) {}
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>>, s32) {}
};

std::vector<Range> Hinted(const WritePreserveDownloadHints& hints, DAddr address, u64 size) {
    std::vector<Range> ranges;
    hints.ForEachRange(address, size, [&](DAddr start, u64 length) {
        ranges.emplace_back(start, length);
    });
    return ranges;
}

TEST_CASE("Write preservation hints learn only actual GPU-written neighbors",
          "[video_core][readback][write_preserve]") {
    WritePreserveDownloadHints hints;
    Common::RangeSet<DAddr> gpu_written;
    const DAddr write = Base + 17 * Page + 128;
    gpu_written.Add(Base, 8 * 1024 * 1024);
    gpu_written.Subtract(write, 64);
    hints.LearnFromWrite(write, 64, gpu_written);
    CHECK(Hinted(hints, Base, 8 * 1024 * 1024) == std::vector<Range>{{write - 128, Page}});
    CHECK(gpu_written.Intersects(write - 1, 1));
    CHECK_FALSE(gpu_written.Intersects(write, 64));
    CHECK(gpu_written.Intersects(write + 64, 1));
    CHECK(hints.BlockCount() == 1);

    hints.Clear();
    gpu_written.Clear();
    gpu_written.Add(write, 64); // Written bytes themselves are not neighbors.
    hints.LearnFromWrite(write, 64, gpu_written);
    CHECK(hints.BlockCount() == 0);
    gpu_written.Clear();
    gpu_written.Add(write - 128 - Page, Page); // An adjacent page is not a shared page.
    hints.LearnFromWrite(write, 64, gpu_written);
    CHECK(hints.BlockCount() == 0);

    gpu_written.Clear();
    gpu_written.Add(Base, 2 * Page);
    gpu_written.Subtract(Base + Page - 4, 8);
    hints.LearnFromWrite(Base + Page - 4, 8, gpu_written);
    CHECK(Hinted(hints, Base, 2 * Page) == std::vector<Range>{{Base, 2 * Page}});
    hints.Clear();
    gpu_written.Clear();
    gpu_written.Add(Base + Page + 4, 1); // Only the trailing page has a GPU-written neighbor.
    hints.LearnFromWrite(Base + Page - 4, 8, gpu_written);
    CHECK(Hinted(hints, Base, 2 * Page) == std::vector<Range>{{Base + Page, Page}});
}

TEST_CASE("Write preservation hints never promote the interior of a large CPU write",
          "[video_core][readback][write_preserve]") {
    WritePreserveDownloadHints hints;
    Common::RangeSet<DAddr> gpu_written;
    const DAddr write = Base + 17;
    constexpr u64 size = 8 * 1024 * 1024 + 100;
    const DAddr last_page = (write + size) & ~(Page - 1);
    gpu_written.Add(Base, size + Page);
    gpu_written.Subtract(write, size);
    hints.LearnFromWrite(write, size, gpu_written);
    CHECK(Hinted(hints, Base, size + Page) == std::vector<Range>{{Base, Page}, {last_page, Page}});
    CHECK(hints.BlockCount() == 2);
    hints.Clear();
    gpu_written.Add(Base, 16 * Page);
    hints.LearnFromWrite(Base, 16 * Page, gpu_written); // Fully overwritten pages need no preservation.
    CHECK(hints.BlockCount() == 0);
}

TEST_CASE("Write preservation hints coalesce and clip across sparse mask boundaries",
          "[video_core][readback][write_preserve]") {
    WritePreserveDownloadHints hints;
    Common::RangeSet<DAddr> gpu_written;
    gpu_written.Add(Base, 4 * Block);
    for (const DAddr page : {Base + 63 * Page, Base + 64 * Page,
                             Base + Block - Page, Base + Block, Base + 3 * Block + Page}) {
        hints.LearnFromWrite(page + 8, 4, gpu_written);
    }
    const DAddr begin = Base + 63 * Page + 19;
    const DAddr end = Base + 3 * Block + Page + 31;
    const std::vector<Range> expected{{begin, 2 * Page - 19},
                                      {Base + Block - Page, 2 * Page}, {end - 31, 31}};
    CHECK(Hinted(hints, begin, end - begin) == expected);
    CHECK(Hinted(hints, begin, end - begin) == expected); // Reads never consume hints.
    CHECK(Hinted(hints, Base + 2 * Block, Block).empty());
    CHECK(hints.BlockCount() == 3);
    hints.Forget(Base + Block, 1);
    CHECK(Hinted(hints, Base + Block - Page, 2 * Page) ==
          std::vector<Range>{{Base + Block - Page, Page}});
    CHECK(hints.BlockCount() == 2);
    hints.Forget(Base, 4 * Block);
    CHECK(hints.BlockCount() == 0);
}

TEST_CASE("Write preservation hints bound their address space and lifetime",
          "[video_core][readback][write_preserve]") {
    WritePreserveDownloadHints hints;
    Common::RangeSet<DAddr> gpu_written;
    constexpr DAddr limit = WritePreserveDownloadHints::AddressLimit;
    gpu_written.Add(limit - Page, Page);
    hints.LearnFromWrite(limit - 5, 1, gpu_written);
    CHECK(Hinted(hints, limit - 17, 17) == std::vector<Range>{{limit - 17, 17}});
    CHECK(hints.BlockCount() == 1);
    for (const Range range : {Range{Base, 0}, Range{limit, 1}, Range{limit - 1, 2},
                              Range{0, std::numeric_limits<u64>::max()},
                              Range{std::numeric_limits<u64>::max(), 2}}) {
        CAPTURE(range.first, range.second);
        hints.LearnFromWrite(range.first, range.second, gpu_written);
        CHECK(Hinted(hints, range.first, range.second).empty());
        hints.Forget(range.first, range.second);
        CHECK(hints.BlockCount() == 1);
    }
    hints.Clear();
    CHECK(hints.BlockCount() == 0);
    CHECK(Hinted(hints, limit - Page, Page).empty());
}

TEST_CASE("Write preservation downloads retain read traps and never promote CPU-read state",
          "[video_core][readback][write_preserve]") {
    DeviceTracker device;
    VideoCommon::MemoryTrackerBase<DeviceTracker> tracker{device};
    WritePreserveDownloadHints hints;
    Common::RangeSet<DAddr> gpu_written;
    gpu_written.Add(Base, 8 * Page);
    tracker.MarkRegionAsPreflushable(Base + Page, Page);
    tracker.MarkRegionAsPreflushable(Base + 4 * Page, Page);
    for (const u64 page : {0, 1, 3, 5, 7}) {
        hints.LearnFromWrite(Base + page * Page + 8, 4, gpu_written);
    }
    std::vector<Range> downloads;
    std::vector<Range> traps;
    VideoCommon::ForEachWrittenDownloadRange(
        tracker, hints, Base, 8 * Page,
        [&](DAddr begin, u64 size) { downloads.emplace_back(begin, size); },
        [&](DAddr begin, u64 size) { traps.emplace_back(begin, size); });
    const std::vector<Range> expected_downloads{{Base, Page}, {Base + Page, Page},
        {Base + 3 * Page, Page}, {Base + 4 * Page, Page}, {Base + 5 * Page, Page},
        {Base + 7 * Page, Page}};
    const std::vector<Range> expected_traps{{Base, Page}, {Base + 2 * Page, 2 * Page},
                                           {Base + 5 * Page, 3 * Page}};
    CHECK(downloads == expected_downloads);
    CHECK(traps == expected_traps);
    CHECK_FALSE(tracker.IsRegionPreflushable(Base, Page));
    CHECK_FALSE(tracker.IsRegionFullyPreflushable(Base + Page - 3, 6));
    CHECK(tracker.IsRegionFullyPreflushable(Base + Page, Page));
    CHECK(gpu_written.Intersects(Base, 8 * Page));

    downloads.clear();
    traps.clear();
    VideoCommon::ForEachWrittenDownloadRange(
        tracker, hints, Base + Page - 3, 6,
        [&](DAddr begin, u64 size) { downloads.emplace_back(begin, size); },
        [&](DAddr begin, u64 size) { traps.emplace_back(begin, size); });
    CHECK(downloads == std::vector<Range>{{Base + Page - 3, 3}, {Base + Page, 3}});
    CHECK(traps == std::vector<Range>{{Base + Page - 3, 3}});
}

} // namespace
