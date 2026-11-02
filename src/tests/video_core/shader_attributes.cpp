// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/attribute.h"

TEST_CASE("All generic shader attribute components are recognized", "[shader_recompiler][attributes]") {
    using namespace Shader::IR;
    for (u32 index = 0; index < NUM_GENERICS; ++index) {
        for (u32 component = 0; component < 4; ++component) {
            const auto attribute = static_cast<Attribute>(static_cast<u64>(Attribute::Generic0X) +
                                                          index * 4 + component);
            REQUIRE(IsGeneric(attribute));
            REQUIRE(GenericAttributeIndex(attribute) == index);
            REQUIRE(GenericAttributeElement(attribute) == component);
        }
    }
    REQUIRE_FALSE(IsGeneric(Attribute::PositionW));
    REQUIRE_FALSE(IsGeneric(Attribute::ColorFrontDiffuseR));
}
