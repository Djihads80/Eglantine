// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/sparse_large_vector.h"

namespace {

using Entry = Common::PageTable::PageEntryData;

const std::size_t EntriesPerHostPage = Common::HostPageSize / sizeof(Entry);

} // namespace

TEST_CASE("Page table block ids do not wrap", "[common]") {
    using Entry = Common::PageTable::PageEntryData;

    REQUIRE(Entry::BlockIdForMapping(0) == 0);
    REQUIRE(Entry::BlockIdForMapping(65534) == 65534);
    REQUIRE(Entry::BlockIdForMapping(65535) == Entry::INVALID_BLOCK_ID);
    REQUIRE(Entry::BlockIdForMapping(65536) == Entry::INVALID_BLOCK_ID);
    REQUIRE(Entry::BlockIdForMapping((std::numeric_limits<u64>::max)()) ==
            Entry::INVALID_BLOCK_ID);
}

TEST_CASE("Page table fast spans reject exhausted block ids", "[common]") {
    using Entry = Common::PageTable::PageEntryData;

    REQUIRE(Entry::CanFastSpan(7, 7));
    REQUIRE_FALSE(Entry::CanFastSpan(7, 8));
    REQUIRE_FALSE(Entry::CanFastSpan(Entry::INVALID_BLOCK_ID, Entry::INVALID_BLOCK_ID));
}

TEST_CASE("SparseLargeVector zeroes every entry of a region", "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 4);
    const std::size_t begin = EntriesPerHostPage / 2;
    const std::size_t end = EntriesPerHostPage * 3 + EntriesPerHostPage / 2;

    entries.CommitRegion(0, entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries.GetUnchecked(i).Store(false, Common::PageType::Memory, 1, 0x1000);
    }

    entries.ZeroRegion(begin, end);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool zeroed = i >= begin && i < end;
        REQUIRE((entries[i].Type() == Common::PageType::Unmapped) == zeroed);
    }
}

// A thread that loaded an entry before an unmap may still store to it afterwards, as
// RasterizerMarkRegionCached does when the GPU uncaches memory the guest is unmapping.
// That store must land in memory the vector still tracks, so the page stays writable and
// the next unmap clears it.
TEST_CASE("SparseLargeVector keeps zeroed pages writable for in-flight stores", "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 2);
    const std::size_t index = EntriesPerHostPage + 3;

    entries.CommitRegion(index, index + 1);
    Entry& entry = entries.GetUnchecked(index);
    entry.Store(false, Common::PageType::RasterizerCachedMemory, 1, 0x1000);

    entries.ZeroRegion(EntriesPerHostPage, EntriesPerHostPage * 2);
    REQUIRE(entries[index].Type() == Common::PageType::Unmapped);

    entry.Store(false, Common::PageType::Memory, 1, 0x1000);
    REQUIRE(entries[index].Type() == Common::PageType::Memory);

    entries.ZeroRegion(EntriesPerHostPage, EntriesPerHostPage * 2);
    REQUIRE(entries[index].Type() == Common::PageType::Unmapped);
}
