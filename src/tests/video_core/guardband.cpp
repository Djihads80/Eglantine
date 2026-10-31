// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/guardband.h"

TEST_CASE("W scale factors follow independently selected clip guardbands", "[guardband]") {
    using namespace Shader;
    for (u32 xy = 0; xy != 2; ++xy) {
        REQUIRE(std::bit_cast<float>(WScaleFactorXY(xy)) == (xy ? 1.0f : 256.0f));
        REQUIRE(WScaleFactorZ(xy, 0) == WScaleFactorXY(xy));
        REQUIRE(std::bit_cast<float>(*WScaleFactorZ(xy, 1)) == 256.0f);
        REQUIRE(std::bit_cast<float>(*WScaleFactorZ(xy, 2)) == 1.0f);
        REQUIRE_FALSE(WScaleFactorZ(xy, 3).has_value());
    }
}
