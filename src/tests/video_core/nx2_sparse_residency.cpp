// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/nx2.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {
constexpr u64 KiB = 1024;
constexpr u64 MiB = 1024 * KiB;

struct SparseRegionScope {
    explicit SparseRegionScope(u64 address_) : address{address_} {}
    ~SparseRegionScope() {
        Tegra::Nx2SparseRegionDestroyed(address);
        Tegra::g_nx2_sparse_low.store(low);
        Tegra::g_nx2_sparse_high.store(high);
    }
    u64 address;
    u64 low = Tegra::g_nx2_sparse_low.load();
    u64 high = Tegra::g_nx2_sparse_high.load();
};
} // namespace

TEST_CASE("GPU-only terrain pools become sparse when explicit tiles arrive", "[nx2_memory]") {
    using namespace VideoCommon;
    constexpr u64 address = 0x9000000000ULL;
    constexpr u64 size = 64 * MiB;
    SparseRegionScope scope{address};
    Tegra::Nx2AddSparseSpan(address, size);
    CHECK_FALSE(Tegra::Nx2InSparseRegion(address, size));

    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    REQUIRE(Tegra::Nx2InSparseRegion(address, size));
    Tegra::Nx2SparseTilesMapped(address, 64 * KiB, 128 * KiB, true);
    const auto ranges = Tegra::Nx2SparseDataRanges(address, size);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges.front() == std::pair<u64, u64>{64 * KiB, 128 * KiB});

    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = PixelFormat::R8_UNORM;
    info.size = {8192, 8192, 1};
    info.resources = {1, 1};
    info.block = {0, 4, 0};
    info.native_tiling = true;
    const auto rects = SparseTileRects(info, ranges, 256, 256, 1);
    REQUIRE_FALSE(rects.empty());
    CHECK(UnswizzledRectsBytes(info, rects) < MiB);

    Tegra::Nx2SparseTilesMapped(address, 64 * KiB, 128 * KiB, false);
    CHECK(Tegra::Nx2SparseDataRanges(address, size).empty());
}

TEST_CASE("Tiled sparse registration preserves existing residency", "[nx2_memory]") {
    constexpr u64 address = 0x9100000000ULL;
    constexpr u64 size = 16 * MiB;
    SparseRegionScope scope{address};
    Tegra::Nx2SparseRegionCreated(address, size, 4 * MiB);
    Tegra::Nx2SparseBlockWritten(address, 2);
    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    CHECK(Tegra::Nx2SparseBlockIsWritten(address, 2));
    CHECK_FALSE(Tegra::Nx2SparseBlockIsWritten(address, 1));

    Tegra::Nx2SparseTilesMapped(address, 8 * MiB, 64 * KiB, true);
    const auto ranges = Tegra::Nx2SparseDataRanges(address, size);
    Tegra::Nx2SparseRegionTiled(address, size, 4 * MiB);
    CHECK(Tegra::Nx2SparseDataRanges(address, size) == ranges);
}
