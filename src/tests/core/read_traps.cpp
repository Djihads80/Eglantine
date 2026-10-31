// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/read_traps.h"

using Core::Memory::ReadTraps;

namespace {
std::vector<u64> Disarmed(ReadTraps& traps, u64 begin, u64 end, u64 flushed_generation) {
    std::vector<u64> pages;
    traps.Disarm(begin, end, flushed_generation, [&](u64 page) { pages.push_back(page); });
    return pages;
}

std::vector<u64> Armed(const ReadTraps& traps) {
    std::vector<u64> pages;
    traps.ForEach(0, ~0ULL, [&](u64 page) { pages.push_back(page); });
    return pages;
}
} // namespace

TEST_CASE("ReadTraps: a flushed read disarms what was armed before it", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 snapshot = traps.Generation();
    REQUIRE(Disarmed(traps, 10, 11, snapshot) == std::vector<u64>{10});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: a re-arm during the flush survives the disarm", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 snapshot = traps.Generation();
    traps.Arm(10, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 10, 11, snapshot).empty());
    REQUIRE(Armed(traps) == std::vector<u64>{10});
    REQUIRE(Disarmed(traps, 10, 11, traps.Generation()) == std::vector<u64>{10});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: a read that started before the arm keeps the trap", "[core][read_traps]") {
    ReadTraps traps;
    const u64 snapshot = traps.Generation();
    traps.Arm(5, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 5, 6, snapshot).empty());
    REQUIRE_FALSE(traps.Empty());
}

TEST_CASE("ReadTraps: disarm and drop stay inside their page range", "[core][read_traps]") {
    ReadTraps traps;
    const u64 generation = traps.NewGeneration();
    for (const u64 page : {1, 2, 3, 10}) {
        traps.Arm(page, generation);
    }
    REQUIRE(Disarmed(traps, 2, 4, generation) == std::vector<u64>{2, 3});
    REQUIRE(Armed(traps) == std::vector<u64>{1, 10});
    traps.Drop(0, 2);
    REQUIRE(Armed(traps) == std::vector<u64>{10});
    traps.Drop(10, 11);
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: re-arming a page counts it once", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(7, traps.NewGeneration());
    traps.Arm(7, traps.NewGeneration());
    traps.Drop(7, 8);
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: clear keeps generations increasing", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(3, traps.NewGeneration());
    const u64 before_clear = traps.Generation();
    traps.Clear();
    REQUIRE(traps.Empty());
    REQUIRE(Armed(traps).empty());
    REQUIRE(traps.NewGeneration() > before_clear);
}

TEST_CASE("ReadTraps: mapping changes invalidate cached read areas without armed pages",
          "[core][read_traps]") {
    ReadTraps traps;
    const u64 before_drop = traps.Generation();
    traps.Drop(10, 11);
    REQUIRE(traps.Generation() > before_drop);
    const u64 before_clear = traps.Generation();
    traps.Clear();
    REQUIRE(traps.Generation() > before_clear);
}
