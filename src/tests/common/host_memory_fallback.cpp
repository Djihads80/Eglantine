// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"

TEST_CASE("HostMemory: Fallback allocation is released", "[common][host_memory]") {
    u8* backing{};
    {
        Common::HostMemory memory{0x1000, 1ULL << 63};
        REQUIRE(memory.VirtualBasePointer() == nullptr);
        backing = memory.BackingBasePointer();
        REQUIRE(backing != nullptr);
        MEMORY_BASIC_INFORMATION info{};
        REQUIRE(VirtualQuery(backing, &info, sizeof(info)) == sizeof(info));
        REQUIRE(info.State == MEM_COMMIT);
    }
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(backing, &info, sizeof(info)) == sizeof(info));
    REQUIRE(info.State == MEM_FREE);
}
#endif
