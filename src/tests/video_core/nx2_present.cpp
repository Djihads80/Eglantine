// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/nx2_present.h"

TEST_CASE("Nx2 presentation honors vertical inversion independently of crop flags", "[nx2_video]") {
    using Service::android::BufferTransformFlags;
    CHECK(Service::Nx2PresentTransform(0x00040003) == BufferTransformFlags::FlipV);
    CHECK(Service::Nx2PresentTransform(0x00040000) == BufferTransformFlags::FlipV);
    CHECK(Service::Nx2PresentTransform(0x00000003) == BufferTransformFlags::Unset);
    CHECK(Service::Nx2PresentTransform(0) == BufferTransformFlags::Unset);
}
