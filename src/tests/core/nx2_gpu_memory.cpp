// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

TEST_CASE("Native GPU unmap counts only removed backing", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x10000, 0x20000, 0x8000);
    REQUIRE(registry.Unmap(0x12000, 0x2000) == 0x2000);
    REQUIRE(registry.Unmap(0x12000, 0x2000) == 0);
    REQUIRE(registry.Unmap(0x10000, 0x10000) == 0x6000);
    REQUIRE(registry.Unmap(0x10000, 0x10000) == 0);
}

TEST_CASE("Native GPU unmap accounts for holes and replacement", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x10000, 0x20000, 0x4000);
    registry.MapAt(0x18000, 0x30000, 0x4000);
    registry.MapAt(0x11000, 0x40000, 0x2000);
    REQUIRE(registry.Unmap(0x10000, 0x10000) == 0x8000);
    REQUIRE(registry.Unmap(0x10000, 0x10000) == 0);
}

TEST_CASE("Native GPU unmap byte count is 64 bit", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x100000000, 0x200000000, 0x100002000);
    REQUIRE(registry.Unmap(0x100000000, 0x100002000) == 0x100002000);
}

TEST_CASE("Native GPU kind changes stay inside one live mapping", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x10000, 0x20000, 0x8000);
    REQUIRE(registry.ContainsMappingRange(0x10000, 0x2000, 0x6000));
    REQUIRE_FALSE(registry.ContainsMappingRange(0x11000, 0, 0x1000));
    REQUIRE_FALSE(registry.ContainsMappingRange(0x10000, 0x7000, 0x2000));
    REQUIRE_FALSE(registry.ContainsMappingRange(0x10000, 0xfffffffffffff000ULL, 0x1000));
    registry.Unmap(0x14000, 0x1000);
    REQUIRE_FALSE(registry.ContainsMappingRange(0x10000, 0x2000, 0x6000));
    REQUIRE(registry.ContainsMappingRange(0x15000, 0, 0x3000));
}

TEST_CASE("Native GPU kind jobs require a mapped range and committed listener", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x10000, 0x20000, 0x8000);
    REQUIRE_FALSE(registry.ModifyKind(0x10000, 0x1000, 6));
    u32 calls{};
    bool commit = true;
    registry.AddListener(&calls, [](u64, u64, u64) {}, [](u64, u64) {}, {},
                         [&](u64 address, u64 size, u8 kind) {
                             ++calls;
                             REQUIRE(address == 0x12000);
                             REQUIRE(size == 0x2000);
                             REQUIRE(kind == 6);
                             return commit;
                         });
    REQUIRE_FALSE(registry.ModifyKind(0x12001, 0x2000, 6));
    REQUIRE_FALSE(registry.ModifyKind(0x12000, 0x2000, 7));
    REQUIRE_FALSE(registry.ModifyKind(0x17000, 0x2000, 6));
    REQUIRE(calls == 0);
    REQUIRE(registry.ModifyKind(0x12000, 0x2000, 6));
    commit = false;
    REQUIRE_FALSE(registry.ModifyKind(0x12000, 0x2000, 6));
    REQUIRE(calls == 2);
    registry.Unmap(0x13000, 0x1000);
    REQUIRE_FALSE(registry.ModifyKind(0x12000, 0x2000, 6));
    REQUIRE(calls == 2);
}

TEST_CASE("Native GPU depth fallbacks commit without changing backing", "[nx2-gpu-memory]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(0x8104b20000, 0x11de800000, 0x2aad000);
    u32 calls{};
    bool commit = true;
    u8 selected_kind = 1;
    registry.AddListener(&calls, [](u64, u64, u64) {}, [](u64, u64) {}, {},
                         [&](u64 address, u64 size, u8 kind) {
                             ++calls;
                             REQUIRE(address == 0x8104b20000);
                             REQUIRE(size == 0x2aac000);
                             REQUIRE(kind == selected_kind); // TU102/GA100 uncompressed depth.
                             return commit;
                         });
    REQUIRE(registry.ModifyKind(0x8104b20000, 0x2aac000, 1));
    REQUIRE(registry.ContainsMappingRange(0x8104b20000, 0, 0x2aad000));
    commit = false;
    REQUIRE_FALSE(registry.ModifyKind(0x8104b20000, 0x2aac000, 1));
    REQUIRE(calls == 2);
    REQUIRE_FALSE(registry.ModifyKind(0x8104b20000, 0x2aac000, 0xb));
    REQUIRE(calls == 2);
    selected_kind = 3; // S8Z24, not compressed kind 0xc.
    commit = true;
    REQUIRE(registry.ModifyKind(0x8104b20000, 0x2aac000, 3));
    REQUIRE_FALSE(registry.ModifyKind(0x8104b20000, 0x2aac000, 0xc));
    REQUIRE(calls == 3);
}
