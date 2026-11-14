// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_allocation_pool.h"

namespace {
using namespace VideoCommon;
ImageInfo PlainImage() {
    ImageInfo info;
    info.native_tiling = true;
    info.type = ImageType::e2D;
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.size = {960, 540, 1};
    info.resources = {.levels = 1, .layers = 1};
    return info;
}
}

TEST_CASE("Image allocation pool gates ownership transfer on GPU completion and exact key",
          "[video_core][image_allocation_pool]") {
    const auto key = ReusableImageAllocationKey(PlainImage()).value();
    ImageAllocationPool<std::unique_ptr<int>> pool{100, 4, 3};
    auto resource = std::make_unique<int>(42);
    auto* address = resource.get();
    CHECK_FALSE(pool.Recycle(key, resource, 40, 7, [](u64 tick) { return tick < 7; }));
    CHECK(resource.get() == address);
    CHECK(pool.Bytes() == 0);
    REQUIRE(pool.Recycle(key, resource, 40, 7, [](u64 tick) { return tick <= 7; }));
    CHECK_FALSE(resource);
    auto different = key;
    ++different.size.width;
    CHECK_FALSE(pool.Acquire(different));
    different = key;
    different.format = PixelFormat::R16G16B16A16_FLOAT;
    CHECK_FALSE(pool.Acquire(different));
    auto acquired = pool.Acquire(key);
    REQUIRE(acquired.get() == address);
    CHECK(*acquired == 42);
    CHECK(pool.Bytes() == 0);
    CHECK(pool.Size() == 0);
    CHECK_FALSE(pool.Acquire(key));
}

TEST_CASE("Image allocation pool bounds memory entries age and pressure",
          "[video_core][image_allocation_pool]") {
    const auto key = ReusableImageAllocationKey(PlainImage()).value();
    const auto done = [](u64) { return true; };
    ImageAllocationPool<std::unique_ptr<int>> pool{100, 2, 2};
    auto a = std::make_unique<int>(1), b = std::make_unique<int>(2), c = std::make_unique<int>(3);
    CHECK_FALSE(pool.Recycle(key, c, 101, 0, done));
    REQUIRE(c);
    REQUIRE(pool.Recycle(key, a, 60, 0, done));
    CHECK_FALSE(pool.Recycle(key, b, 0, 0, done));
    REQUIRE(b);
    REQUIRE(pool.Recycle(key, b, 40, 0, done));
    CHECK(pool.Bytes() == 100);
    REQUIRE(pool.Recycle(key, c, 30, 0, done));
    CHECK_FALSE(c);
    CHECK(pool.Bytes() == 70);
    CHECK(pool.Size() == 2);
    CHECK(pool.Evicted() == 1);
    pool.Tick(false);
    pool.Tick(false);
    CHECK(pool.Size() == 2);
    pool.Tick(false);
    CHECK(pool.Bytes() == 0);
    CHECK(pool.Size() == 0);
    auto d = std::make_unique<int>(4);
    REQUIRE(pool.Recycle(key, d, 50, 0, done));
    pool.Tick(true);
    CHECK(pool.Bytes() == 0);
    auto e = std::make_unique<int>(5);
    CHECK_FALSE(pool.Recycle(key, e, 50, 0, done));
    REQUIRE(e);
    pool.Tick(false);
    CHECK(pool.Recycle(key, e, 50, 0, done));
    CHECK(pool.Bytes() == 50);
}

TEST_CASE("Reusable image allocation keys reject alternate host allocation requirements",
          "[video_core][image_allocation_pool]") {
    auto info = PlainImage();
    REQUIRE(ReusableImageAllocationKey(info));
    SECTION("Legacy layout") { info.native_tiling = false; }
    SECTION("Mip chains") { info.resources.levels = 2; }
    SECTION("Arrays") { info.resources.layers = 2; }
    SECTION("Multisampling") { info.num_samples = 4; }
    SECTION("Volumes") { info.type = ImageType::e3D; info.size.depth = 4; }
    SECTION("Sparse") { info.is_sparse = true; }
    SECTION("Compressed") { info.format = PixelFormat::BC1_RGBA_UNORM; }
    SECTION("Empty") { info.size.width = 0; }
    CHECK_FALSE(ReusableImageAllocationKey(info));
}

TEST_CASE("Image allocation pool preserves other keys and releases every owner once",
          "[video_core][image_allocation_pool]") {
    struct Allocation {
        int id;
        int& released;
        ~Allocation() { ++released; }
    };
    int released = 0;
    ImageAllocationPool<std::unique_ptr<Allocation>> pool{100, 2, 3};
    auto a = std::unique_ptr<Allocation>{new Allocation{1, released}};
    auto b = std::unique_ptr<Allocation>{new Allocation{2, released}};
    auto c = std::unique_ptr<Allocation>{new Allocation{3, released}};
    auto first = ReusableImageAllocationKey(PlainImage()).value();
    auto second = first;
    ++second.size.height;
    auto third = first;
    ++third.size.width;
    const auto done = [](u64) { return true; };
    REQUIRE(pool.Recycle(first, a, 10, 0, done));
    REQUIRE(pool.Recycle(second, b, 20, 0, done));
    auto acquired = pool.Acquire(second);
    REQUIRE(acquired);
    CHECK(acquired->id == 2);
    CHECK(pool.Bytes() == 10);
    CHECK(released == 0);
    REQUIRE(pool.Recycle(third, c, 30, 0, done));
    CHECK(pool.Bytes() == 40);
    pool.Tick(true);
    CHECK(released == 2);
    acquired.reset();
    CHECK(released == 3);
}

TEST_CASE("Full image allocation pool never evicts for unfinished work",
          "[video_core][image_allocation_pool]") {
    const auto key = ReusableImageAllocationKey(PlainImage()).value();
    ImageAllocationPool<std::unique_ptr<int>> pool{100, 1, 3};
    auto a = std::make_unique<int>(1), b = std::make_unique<int>(2);
    REQUIRE(pool.Recycle(key, a, 10, 1, [](u64) { return true; }));
    CHECK_FALSE(pool.Recycle(key, b, 10, 2, [](u64) { return false; }));
    REQUIRE(b);
    CHECK(pool.Evicted() == 0);
    REQUIRE(pool.Recycle(key, b, 10, 2, [](u64) { return true; }));
    CHECK(pool.Evicted() == 1);
    CHECK(pool.Bytes() == 10);
    CHECK(pool.Size() == 1);
    auto acquired = pool.Acquire(key);
    REQUIRE(acquired);
    CHECK(*acquired == 2);
}

TEST_CASE("Image allocation pool bounds whole pinned memory blocks without double charging",
          "[video_core][image_allocation_pool]") {
    const auto key = ReusableImageAllocationKey(PlainImage()).value();
    ImageAllocationPool<std::unique_ptr<int>> pool{100, 4, 3};
    const auto done = [](u64) { return true; };
    auto a = std::make_unique<int>(1), b = std::make_unique<int>(2);
    auto c = std::make_unique<int>(3), d = std::make_unique<int>(4);
    REQUIRE(pool.Recycle(key, a, 10, 0, done, 101, 60));
    REQUIRE(pool.Recycle(key, b, 20, 0, done, 101, 60));
    CHECK(pool.Bytes() == 60);
    CHECK(pool.AllocationBytes() == 30);
    auto acquired = pool.Acquire(key);
    REQUIRE(acquired);
    CHECK(*acquired == 1);
    CHECK(pool.Bytes() == 60);
    CHECK(pool.AllocationBytes() == 20);
    REQUIRE(pool.Recycle(key, c, 10, 0, done, 202, 60));
    CHECK(pool.Evicted() == 1);
    CHECK(pool.Bytes() == 60);
    CHECK(pool.AllocationBytes() == 10);
    REQUIRE(pool.Recycle(key, d, 5, 0, done, 0, 50)); // Dedicated allocations charge individually.
    CHECK(pool.Bytes() == 50);
    CHECK(pool.AllocationBytes() == 5);
    pool.Tick(true);
    CHECK(pool.Bytes() == 0);
    CHECK(pool.AllocationBytes() == 0);
    pool.Tick(false);
    auto too_large = std::make_unique<int>(5);
    CHECK_FALSE(pool.Recycle(key, too_large, 1, 0, done, 303, 101));
    REQUIRE(too_large);
}
