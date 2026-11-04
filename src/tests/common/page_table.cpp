// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"

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
