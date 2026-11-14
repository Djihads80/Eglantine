// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "common/literals.h"
#include "video_core/texture_cache/memory_budget.h"

using namespace Common::Literals;

TEST_CASE("Idle texture retention never delays pressure-driven collection",
          "[video_core][texture_cache_budget]") {
    using VideoCommon::TextureCacheCollectionAge;
    for (const u64 idle_age : {u64{0}, u64{50}, u64{600}, u64{3600}, ~u64{0}}) {
        CAPTURE(idle_age);
        CHECK(TextureCacheCollectionAge(true, false, idle_age) == 25);
        CHECK(TextureCacheCollectionAge(true, true, idle_age) == 10);
    }
    CHECK(TextureCacheCollectionAge(false, false, 50) == 50);
    CHECK(TextureCacheCollectionAge(false, false, 600) == 600);
    CHECK(TextureCacheCollectionAge(false, false, 0) == 50);
    CHECK(TextureCacheCollectionAge(false, false, ~u64{0}) == 3600);
}

TEST_CASE("Native texture cache limits follow available budget and retain small card limits",
          "[video_core][texture_cache_budget]") {
    using VideoCommon::NativeTextureCacheCriticalLimit;
    CHECK(NativeTextureCacheCriticalLimit(13_GiB, 14_GiB, 5_GiB) == 13_GiB);
    CHECK(NativeTextureCacheCriticalLimit(13_GiB + 512_MiB, 14_GiB + 512_MiB, 9_GiB) == 13_GiB + 512_MiB);
    CHECK(NativeTextureCacheCriticalLimit(6_GiB, 7_GiB, 5_GiB) == 5_GiB);
    CHECK(NativeTextureCacheCriticalLimit(6_GiB, 8_GiB, 5_GiB) == 5_GiB);
    CHECK(NativeTextureCacheCriticalLimit(3_GiB, 0, 5_GiB) == 3_GiB);
    CHECK(NativeTextureCacheCriticalLimit(4_GiB, 14_GiB, 5_GiB) == 4_GiB);
    CHECK(NativeTextureCacheCriticalLimit(256_MiB, 1_GiB, 5_GiB) == 256_MiB);

    u64 previous{};
    for (u64 mib = 0; mib <= 32768; mib += 17) {
        const u64 budget = mib << 20;
        const u64 critical_budget = budget - (std::min)(budget, u64{2_GiB});
        const u64 limit = NativeTextureCacheCriticalLimit(critical_budget, budget, 5_GiB);
        CAPTURE(mib);
        CHECK(limit <= critical_budget);
        CHECK(limit >= (std::min)(critical_budget, u64{5_GiB}));
        CHECK(limit >= previous);
        if (budget <= 8_GiB) {
            CHECK(limit == (std::min)(critical_budget, u64{5_GiB}));
        }
        previous = limit;
    }
}
