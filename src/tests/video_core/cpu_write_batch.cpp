// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/cpu_write_batch.h"

namespace {

using VideoCommon::CpuWriteRange;
using VideoCommon::PrepareCpuWriteBatch;
using VideoCommon::WritePreserveDownloadHints;
constexpr u64 Page = WritePreserveDownloadHints::PageSize;
constexpr DAddr Base = 16 * 1024 * 1024;

std::vector<CpuWriteRange> Ranges(const Common::RangeSet<DAddr>& ranges) {
    std::vector<CpuWriteRange> result;
    ranges.ForEach([&](DAddr begin, DAddr end) { result.emplace_back(begin, end - begin); });
    return result;
}

TEST_CASE("CPU write batch excludes later writes before preserving a shared GPU page",
          "[video_core][readback][cpu_write_batch]") {
    Common::RangeSet<DAddr> gpu_written;
    Common::RangeSet<DAddr> pending_download;
    WritePreserveDownloadHints hints;
    gpu_written.Add(Base, 2 * Page);
    pending_download.Add(Base, 2 * Page);
    std::array<u8, 2 * Page> host_gpu;
    std::array<u8, 2 * Page> guest;
    host_gpu.fill(0x47);
    guest.fill(0x11);
    const std::array<CpuWriteRange, 2> writes{{{Base + 64, 64}, {Base + 256, 64}}};
    std::fill_n(guest.begin() + 64, 64, 0xa1);
    std::fill_n(guest.begin() + 256, 64, 0xb2);

    const auto pages = PrepareCpuWriteBatch(writes, gpu_written, hints,
        [&](DAddr address, u64 size) { pending_download.Subtract(address, size); });
    CHECK(Ranges(pages) == std::vector<CpuWriteRange>{{Base, Page}});
    CHECK_FALSE(gpu_written.Intersects(Base + 64, 64));
    CHECK_FALSE(gpu_written.Intersects(Base + 256, 64));
    CHECK_FALSE(pending_download.Intersects(Base + 64, 64));
    CHECK_FALSE(pending_download.Intersects(Base + 256, 64));

    // Use the production plan and surviving dirty intervals, as the readback consumes them.
    // In the old per-write order, preserving A's neighbors copied 0x47 over B's 0xb2.
    u64 copied_bytes = 0;
    pages.ForEach([&](DAddr begin, DAddr end) {
        gpu_written.ForEachInRange(begin, end - begin, [&](DAddr first, DAddr last) {
            std::copy(host_gpu.begin() + first - Base, host_gpu.begin() + last - Base,
                      guest.begin() + first - Base);
            copied_bytes += last - first;
        });
    });
    CHECK(copied_bytes == Page - 128);
    for (size_t i = 0; i < guest.size(); ++i) {
        const u8 expected = i >= Page ? 0x11 : i >= 64 && i < 128 ? 0xa1 :
                            i >= 256 && i < 320 ? 0xb2 : 0x47;
        REQUIRE(guest[i] == expected);
    }
    CHECK(gpu_written.Intersects(Base + Page, Page));
    CHECK(pending_download.Intersects(Base + Page, Page));
}

TEST_CASE("CPU write batch merges overlapping writes before learning preservation hints",
          "[video_core][readback][cpu_write_batch]") {
    Common::RangeSet<DAddr> gpu_written;
    Common::RangeSet<DAddr> pending;
    WritePreserveDownloadHints hints;
    gpu_written.Add(Base, 2 * Page);
    pending.Add(Base, 2 * Page);
    const std::array<CpuWriteRange, 4> writes{{
        {Base, Page / 2}, {Base + Page / 4, Page / 2},
        {Base + Page / 2, Page / 2}, {Base, Page / 2}}};
    const auto pages = PrepareCpuWriteBatch(writes, gpu_written, hints,
        [&](DAddr address, u64 size) { pending.Subtract(address, size); });
    CHECK(pages.Empty());
    CHECK(hints.BlockCount() == 0);
    CHECK(Ranges(gpu_written) == std::vector<CpuWriteRange>{{Base + Page, Page}});
    CHECK(Ranges(pending) == Ranges(gpu_written));
}

TEST_CASE("CPU write batch preserves exact dirty neighbors across pages and sparse gaps",
          "[video_core][readback][cpu_write_batch]") {
    Common::RangeSet<DAddr> gpu_written;
    WritePreserveDownloadHints hints;
    gpu_written.Add(Base + 8, 8);
    gpu_written.Add(Base + Page - 16, Page + 32);
    gpu_written.Add(Base + 3 * Page + 300, 4);
    gpu_written.Add(Base + 8 * Page, 64);
    const std::array<CpuWriteRange, 2> writes{{
        {Base + Page - 8, Page + 16}, {Base + 3 * Page + 128, 64}}};
    const auto pages = PrepareCpuWriteBatch(writes, gpu_written, hints,
                                           [](DAddr, u64) {});
    CHECK(Ranges(pages) == std::vector<CpuWriteRange>{{Base, 4 * Page}});
    CHECK(Ranges(gpu_written) == std::vector<CpuWriteRange>{
        {Base + 8, 8}, {Base + Page - 16, 8}, {Base + 2 * Page + 8, 8},
        {Base + 3 * Page + 300, 4}, {Base + 8 * Page, 64}});
    std::vector<CpuWriteRange> hinted;
    hints.ForEachRange(Base, 9 * Page,
                      [&](DAddr address, u64 size) { hinted.emplace_back(address, size); });
    CHECK(hinted == std::vector<CpuWriteRange>{{Base, Page}, {Base + 2 * Page, 2 * Page}});
}

TEST_CASE("CPU write batch cancels snapshots after an earlier pop clears the dirty mark",
          "[video_core][readback][cpu_write_batch]") {
    Common::RangeSet<DAddr> gpu_written;
    std::array<Common::RangeSet<DAddr>, 2> pending;
    WritePreserveDownloadHints hints;
    constexpr CpuWriteRange write_a{Base + 64, 64};
    constexpr CpuWriteRange write_b{Base + 256, 64};
    std::array<u8, Page> guest;
    std::array<u8, Page> snapshot;
    guest.fill(0x11);
    snapshot.fill(0x47);
    gpu_written.Add(write_a.first, write_a.second);
    gpu_written.Add(write_b.first, write_b.second);
    pending[0].Add(write_a.first, write_a.second);
    // B's later page readback also captured A before the first snapshot was popped.
    pending[1].Add(write_a.first, write_a.second);
    pending[1].Add(write_b.first, write_b.second);
    const auto pop = [&](Common::RangeSet<DAddr>& ranges) {
        ranges.ForEach([&](DAddr begin, DAddr end) {
            std::copy(snapshot.begin() + (begin - Base), snapshot.begin() + (end - Base),
                      guest.begin() + (begin - Base));
            gpu_written.Subtract(begin, end - begin);
        });
        ranges.Clear();
    };
    pop(pending[0]);
    REQUIRE_FALSE(gpu_written.Intersects(write_a.first, write_a.second));
    REQUIRE(pending[1].Intersects(write_a.first, write_a.second));
    REQUIRE(gpu_written.Intersects(write_b.first, write_b.second));

    std::fill_n(guest.begin() + (write_a.first - Base), write_a.second, 0xa5);
    const std::array<CpuWriteRange, 1> writes{{write_a}};
    size_t discarded = 0;
    const auto pages = PrepareCpuWriteBatch(
        writes, gpu_written, hints, [&](DAddr address, u64 size) {
            ++discarded;
            for (auto& ranges : pending) {
                ranges.Subtract(address, size);
            }
        });
    REQUIRE(discarded == 1);
    REQUIRE_FALSE(pending[1].Intersects(write_a.first, write_a.second));
    CHECK(Ranges(pending[1]) == std::vector<CpuWriteRange>{write_b});
    CHECK(Ranges(gpu_written) == std::vector<CpuWriteRange>{write_b});
    CHECK(Ranges(pages) == std::vector<CpuWriteRange>{{Base, Page}});

    pop(pending[1]);
    for (size_t i = 0; i < guest.size(); ++i) {
        const u8 expected = i >= 64 && i < 128 ? 0xa5 :
                            i >= 256 && i < 320 ? 0x47 : 0x11;
        REQUIRE(guest[i] == expected);
    }
}

TEST_CASE("CPU write batch bounds address arithmetic and leaves unrelated data dirty",
          "[video_core][readback][cpu_write_batch]") {
    constexpr DAddr limit = WritePreserveDownloadHints::AddressLimit;
    Common::RangeSet<DAddr> gpu_written;
    WritePreserveDownloadHints hints;
    gpu_written.Add(limit - Page, Page);
    const std::array<CpuWriteRange, 6> writes{{
        {limit - 16, 16}, {Base, 0}, {0, 64}, {limit, 1},
        {limit - 1, 2}, {std::numeric_limits<u64>::max(), 64}}};
    size_t discarded = 0;
    const auto pages = PrepareCpuWriteBatch(writes, gpu_written, hints,
                                           [&](DAddr, u64) { ++discarded; });
    CHECK(discarded == 1);
    CHECK(Ranges(pages) == std::vector<CpuWriteRange>{{limit - Page, Page}});
    CHECK(Ranges(gpu_written) == std::vector<CpuWriteRange>{{limit - Page, Page - 16}});
    CHECK(hints.BlockCount() == 1);
}

} // namespace
