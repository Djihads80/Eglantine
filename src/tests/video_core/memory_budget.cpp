// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "video_core/vulkan_common/memory_budget.h"

using namespace Common::Literals;

TEST_CASE("Host commit pressure limits reclaimable cache budgets", "[memory_budget]") {
    using Vulkan::LimitBudgetForHostCommit;
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 20_GiB, 4_GiB) == 10_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) == 9_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 4_GiB, 4_GiB) == 8_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 3_GiB, 4_GiB) == 7_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 0, 4_GiB) == 4_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 1_GiB, 0, 4_GiB) == 0);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) ==
            LimitBudgetForHostCommit(10_GiB, 7_GiB, 6_GiB, 4_GiB));
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 20_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 3_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, std::numeric_limits<u64>::max(), 4_GiB) ==
            10_GiB);
}
