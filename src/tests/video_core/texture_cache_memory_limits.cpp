// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/memory_limits.h"

using namespace Common::Literals;
namespace TCM = VideoCommon::TextureCacheMemory;

TEST_CASE("Texture cache memory limit: Auto is three quarters of the device", "[video_core]") {
    CHECK(TCM::ResolveLimit(0, 16_GiB) == 12_GiB);
    CHECK(TCM::ResolveLimit(0, 8_GiB) == 6_GiB);
    CHECK(TCM::RecommendedLimit(0) == 0);
}

TEST_CASE("Texture cache memory limit: explicit values are clamped to the device", "[video_core]") {
    CHECK(TCM::ResolveLimit(4096, 16_GiB) == 4_GiB);
    CHECK(TCM::ResolveLimit(512, 16_GiB) == TCM::MinLimit);
    CHECK(TCM::ResolveLimit(65536, 16_GiB) == 16_GiB);
    // A device under the minimum is the ceiling.
    CHECK(TCM::ResolveLimit(512, 512_MiB) == 512_MiB);
}

TEST_CASE("Texture cache memory limit: unknown device memory", "[video_core]") {
    CHECK(TCM::ResolveLimit(0, 0) == ~u64{0});
    CHECK(TCM::ResolveLimit(3000, 0) == 3000_MiB);
}

TEST_CASE("Texture cache memory limit: warning below the recommendation", "[video_core]") {
    CHECK_FALSE(TCM::IsBelowRecommended(0, 8_GiB));
    CHECK(TCM::IsBelowRecommended(4096, 8_GiB));
    CHECK_FALSE(TCM::IsBelowRecommended(6144, 8_GiB));
    CHECK_FALSE(TCM::IsBelowRecommended(8192, 8_GiB));
    CHECK_FALSE(TCM::IsBelowRecommended(1024, 0));
}
