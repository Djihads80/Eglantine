// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "core/hle/kernel/k_page_heap.h"

using namespace Common::Literals;

TEST_CASE("Linear graphics backing allocation reuses released pages", "[kernel][heap][page_reuse]") {
    constexpr size_t HeapSize = 256_MiB;
    constexpr size_t BlockSize = 4_MiB;
    constexpr auto HeapStart = Kernel::KPhysicalAddress{0x80000000};
    constexpr auto BlockIndex = Kernel::KPageHeap::GetBlockIndex(BlockSize / Kernel::PageSize);
    static_assert(Kernel::KPageHeap::GetBlockSize(BlockIndex) == BlockSize);

    Kernel::KPageHeap heap;
    const auto management_size = Kernel::KPageHeap::CalculateManagementOverheadSize(HeapSize);
    heap.Initialize(HeapStart, HeapSize,
                    Kernel::KVirtualAddress{std::numeric_limits<uintptr_t>::max() - management_size},
                    management_size);
    heap.Free(HeapStart, HeapSize / Kernel::PageSize);

    std::array<Kernel::KPhysicalAddress, 8> live;
    std::set<uintptr_t> touched;
    for (auto& address : live) {
        address = heap.AllocateBlock(BlockIndex, false);
        REQUIRE(address != 0);
        REQUIRE(GetInteger(address) % BlockSize == 0);
        REQUIRE(touched.insert(GetInteger(address)).second);
    }
    REQUIRE(heap.GetFreeSize() == HeapSize - live.size() * BlockSize);

    for (size_t iteration = 0; iteration < 2048; ++iteration) {
        auto& address = live[iteration % live.size()];
        const auto released = address;
        heap.Free(released, BlockSize / Kernel::PageSize);
        address = heap.AllocateBlock(BlockIndex, false);
        REQUIRE(address == released);
        touched.insert(GetInteger(address));
        REQUIRE(heap.GetFreeSize() == HeapSize - live.size() * BlockSize);
    }
    REQUIRE(touched.size() == live.size());

    for (const auto address : live) {
        heap.Free(address, BlockSize / Kernel::PageSize);
    }
    REQUIRE(heap.GetFreeSize() == HeapSize);
}
