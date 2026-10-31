// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"
#include "common/literals.h"

using namespace Common::Literals;

TEST_CASE("Deferred backing commits shared granules without committing unused ranges",
          "[host_memory][host_commit]") {
    constexpr size_t GranuleSize = 64_KiB;
    Common::HostMemory memory{8_MiB, 1_GiB, Common::HostMemory::BackingMode::Deferred};
    REQUIRE(memory.BackingBasePointer() != nullptr);
    REQUIRE(memory.VirtualBasePointer() != nullptr);

    MEMORY_BASIC_INFORMATION region{};
    REQUIRE(VirtualQuery(memory.BackingBasePointer(), &region, sizeof(region)) == sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    REQUIRE(region.RegionSize == 8_MiB);

    memory.MarkUsed(0x1000, 0x1000);
    REQUIRE(VirtualQuery(memory.BackingBasePointer(), &region, sizeof(region)) == sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(region.RegionSize == GranuleSize);

    memory.Map(0x10000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    memory.Map(0x30000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    volatile u8* const first = memory.VirtualBasePointer() + 0x10000;
    volatile u8* const second = memory.VirtualBasePointer() + 0x30000;
    first[0x1000] = 42;
    REQUIRE(memory.BackingBasePointer()[0x1000] == 42);
    REQUIRE(second[0x1000] == 42);

    memory.MarkUsed(0x9000, 0x1000);
    REQUIRE(second[0x9000] == 0);
    second[0x9000] = 73;
    REQUIRE(first[0x9000] == 73);

    memory.Map(0x50000, 2 * GranuleSize, GranuleSize,
               Common::MemoryPermission::ReadWrite, false);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x50000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    memory.MarkUsed(2 * GranuleSize + 0x1000, 0x1000);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 2 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(memory.BackingBasePointer()[2 * GranuleSize + 0x1000] == 0);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x50000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    memory.VirtualBasePointer()[0x51000] = 91;
    REQUIRE(memory.BackingBasePointer()[2 * GranuleSize + 0x1000] == 91);

    memory.ClearBackingRegion(4 * GranuleSize, 0x1000, 0);
    REQUIRE(memory.BackingBasePointer()[4 * GranuleSize] == 0);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 3 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);

    memory.Unmap(0x10000, GranuleSize, false);
    REQUIRE(second[0x1000] == 42);
    memory.Map(0x10000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    REQUIRE(first[0x9000] == 73);
    memory.MarkDirty(0, GranuleSize);
    memory.ClearBackingRegion(0, GranuleSize, 0);
    REQUIRE(first[0x1000] == 0);
    REQUIRE(second[0x9000] == 0);
}
