// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nvdrv/devices/nx2_gpu_mapping_table.h"

using Service::Nvidia::Devices::Nx2GpuMappingTable;

TEST_CASE("Nx2 native image mappings validate bound page extents", "[nx2_mapping]") {
    const auto range = Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x1100, 0x1000);
    REQUIRE(range);
    CHECK(range->gpu == 0x11000);
    CHECK(range->size == 0x2000);
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x3000, 0x1001));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x5000, 1));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10001, 0x4000, 0, 0x1000));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0, 0x4000, 0, 0x1000));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0, 0));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(
        std::numeric_limits<u64>::max() - 0x1000, 0x2000, 0, 0x1000));
}

TEST_CASE("Nx2 native image leases share mappings across handles", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto first = table.Acquire(0x10000, 0x3000);
    const auto second = table.Acquire(0x11000, 0x1000);
    REQUIRE(first.success);
    REQUIRE(second.success);
    REQUIRE(first.changes.size() == 1);
    CHECK(first.changes[0].map);
    CHECK(first.changes[0].native);
    CHECK(second.changes.empty());
    const auto released = table.Release(first.lease);
    REQUIRE(released.changes.size() == 2);
    CHECK_FALSE(released.changes[0].map);
    CHECK_FALSE(released.changes[1].map);
    CHECK(table.UsesBacking(0x10000, 0x3000));
    CHECK(table.Release(second.lease).changes.size() == 1);
    CHECK_FALSE(table.UsesBacking(0x10000, 0x3000));
    CHECK_FALSE(table.Release(second.lease).success);
}

TEST_CASE("Nx2 native image leases borrow existing GPU mappings", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    REQUIRE(table.Map(0x10000, 0x10000, 0x4000).success);
    const auto lease = table.Acquire(0x11000, 0x1000);
    REQUIRE(lease.success);
    CHECK(lease.changes.empty());
    CHECK(table.Release(lease.lease).changes.empty());
    CHECK(table.UsesBacking(0x10000, 0x4000));
    CHECK(table.Unmap(0x10000, 0x4000).changes.size() == 1);
}

TEST_CASE("Nx2 native leases survive external unmaps and reject conflicting aliases", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    REQUIRE(table.Map(0x10000, 0x10000, 0x3000).success);
    const auto lease = table.Acquire(0x11000, 0x1000);
    REQUIRE(lease.success);
    const auto unmap = table.Unmap(0x10000, 0x3000);
    REQUIRE(unmap.changes.size() == 2);
    CHECK_FALSE(table.Map(0x11000, 0x90000, 0x1000).success);
    CHECK(table.UsesBacking(0x11000, 0x1000));
    CHECK(table.Release(lease.lease).changes.size() == 1);
    REQUIRE(table.Map(0x11000, 0x90000, 0x1000).success);
    CHECK_FALSE(table.Acquire(0x11000, 0x1000).success);
}

TEST_CASE("Nx2 native backing remains borrowed by other GPU aliases", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto lease = table.Acquire(0x10000, 0x2000);
    REQUIRE(lease.success);
    REQUIRE(table.Map(0x90000, 0x10000, 0x2000).success);
    REQUIRE(table.Map(0x80000, 0x10000, 0x2000).success);
    table.Release(lease.lease);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x80000, 0x1000);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x81000, 0x1000);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x90000, 0x2000);
    CHECK_FALSE(table.UsesBacking(0x10000, 0x2000));
}

TEST_CASE("Nx2 native remaps emit ordered removal then replacement", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto first = table.Acquire(0x10000, 0x1000);
    const auto unmap = table.Release(first.lease);
    const auto second = table.Acquire(0x10000, 0x1000);
    REQUIRE(unmap.changes.size() == 1);
    REQUIRE(second.changes.size() == 1);
    CHECK_FALSE(unmap.changes[0].map);
    CHECK(second.changes[0].map);
    CHECK_FALSE(table.Release(first.lease).success);
    CHECK(table.UsesBacking(0x10000, 0x1000));
    table.Reset();
    const auto next_session = table.Acquire(0x10000, 0x1000);
    CHECK(next_session.lease != second.lease);
    CHECK_FALSE(table.Release(second.lease).success);
    CHECK(table.UsesBacking(0x10000, 0x1000));
}
