// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "core/hle/kernel/k_address_space_info.h"
#include "core/hle/kernel/k_memory_layout.h"

using namespace Common::Literals;

TEST_CASE("Switch 2 processes reserve a heap beyond the legacy 8 GiB ceiling", "[kernel][heap]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap, true) == 12_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap, true) >= 0x261600000ULL);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap) == 8_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap, false) == 8_GiB);
    REQUIRE(Space::GetAddressSpaceSize(36, Type::Heap, true) == 8_GiB);
    REQUIRE(Space::GetAddressSpaceSize(32, Type::Heap, true) == 1_GiB);

    for (const auto type : {Type::Map39Bit, Type::MapSmall, Type::Alias, Type::Stack}) {
        REQUIRE(Space::GetAddressSpaceSize(39, type, true) ==
                Space::GetAddressSpaceSize(39, type, false));
    }
}

TEST_CASE("Kernel page table heap covers the Switch 2 main memory", "[kernel][heap]") {
    static_assert(Kernel::KernelPageTableHeapSize ==
                  Kernel::GetMaximumOverheadSize(Kernel::MainMemorySizeMaxSwitch2));
    static_assert(Kernel::KernelPageTableHeapSize >
                  Kernel::GetMaximumOverheadSize(Kernel::MainMemorySizeMax));
    REQUIRE(Kernel::KernelPageTableHeapSize == (12 + 6144) * Kernel::PageSize);
    REQUIRE(Kernel::MainMemorySizeMax == 8_GiB);
    REQUIRE(Kernel::MainMemorySizeMaxSwitch2 == 12_GiB);
}
