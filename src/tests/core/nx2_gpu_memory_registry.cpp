// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

TEST_CASE("NX2 sparse aliases cover the entire tile backing", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf0000000;
    constexpr u64 device = 0x90000000;
    registry.RecordDeviceRange(base, 0x4000, device);
    registry.RecordDeviceRange(base + 0x1000, 0x1000, device + 0x10000);
    SCOPE_EXIT {
        registry.ReleaseBacking(base, 0x4000);
        registry.ReleaseBacking(base + 0x1000, 0x1000);
    };

    // A small tile fits the most recent mapping; a tile crossing its end must use the outer one.
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x100) == device + 0x10800);
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x1000) == device + 0x1800);
    CHECK(registry.DeviceAddressOf(base, 0x4000) == device);
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x3000, 0x2000));
    CHECK_FALSE(registry.DeviceAddressOf(base, 0));
    CHECK_FALSE(registry.DeviceAddressOf(base, std::numeric_limits<u64>::max()));
    CHECK_FALSE(registry.DeviceAddressOf(std::numeric_limits<u64>::max(), 1));
}

TEST_CASE("NX2 released backing cannot supply a sparse alias", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf1000000;
    registry.RecordDeviceRange(base, 0x1000, 0xa0000000);
    REQUIRE(registry.DeviceAddressOf(base, 0x1000));
    registry.ReleaseBacking(base, 0x1000);
    CHECK_FALSE(registry.DeviceAddressOf(base, 1));
}
