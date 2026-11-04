// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/indirect_reset.h"

TEST_CASE("Indirect reset validates list bounds", "[video_core][indirect_reset]") {
    using VideoCommon::IndirectResetRange;
    const auto valid = IndirectResetRange(0x1000, 800, 4, 96);
    REQUIRE(valid);
    REQUIRE(valid->begin == 0x1020);
    REQUIRE(valid->size == 768);
    REQUIRE_FALSE(IndirectResetRange(0, 800, 0, 1));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 800, 0, 0));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 800, 5, 96));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 0xffffffffU, 0, (1U << 20) + 1));
    REQUIRE_FALSE(IndirectResetRange((std::numeric_limits<u64>::max)() - 3, 800, 0, 1));
    REQUIRE_FALSE(IndirectResetRange((std::numeric_limits<u64>::max)() - 3, 800, 1, 1));
}
