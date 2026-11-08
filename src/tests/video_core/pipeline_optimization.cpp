// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_optimization.h"

TEST_CASE("Only known AMD proprietary shaders disable pipeline optimization",
          "[video_core][vulkan][pipeline_optimization]") {
    constexpr std::array<u64, 2> hashes{0xeff3ccbe1438679dULL, 0xf9ea14a22394f5b1ULL};
    for (const u64 hash : hashes) {
        CHECK(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash));
        for (const auto driver : {VK_DRIVER_ID_AMD_OPEN_SOURCE, VK_DRIVER_ID_MESA_RADV,
                                  VK_DRIVER_ID_NVIDIA_PROPRIETARY,
                                  VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
                                  VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_DRIVER_ID_MESA_TURNIP}) {
            CHECK_FALSE(Vulkan::BuildsUnoptimized(driver, hash));
        }
        CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash ^ 1));
    }
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0));
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0x3c2193d0166610b3ULL));
    for (const u64 hash : {0x8c29514395041853ULL, 0xc332d87961d23b13ULL,
                           0xb2f024003969e2dcULL}) {
        CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash));
    }
}
