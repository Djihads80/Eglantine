// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace {

constexpr u64 SPACE = 512;

std::array<bool, SPACE> Contents(const Common::RangeSet<u64>& set) {
    std::array<bool, SPACE> contents{};
    set.ForEach([&](u64 begin, u64 end) {
        for (u64 address = begin; address < end; ++address) {
            contents[address] = true;
        }
    });
    return contents;
}

} // Anonymous namespace

TEST_CASE("RangeSet::Subtract leaves neighbours of the range alone", "[range_sets]") {
    Common::RangeSet<u64> set;
    set.Add(100, 50);
    set.Subtract(150, 10);
    set.Subtract(90, 10);
    set.Subtract(0, 0);
    set.Subtract(120, 0);
    std::array<bool, SPACE> expected{};
    for (u64 address = 100; address < 150; ++address) {
        expected[address] = true;
    }
    REQUIRE(Contents(set) == expected);
    set.Subtract(149, 1);
    set.Subtract(90, 11);
    expected[149] = false;
    expected[100] = false;
    REQUIRE(Contents(set) == expected);

    Common::RangeSet<u64> empty;
    empty.Subtract(0, SPACE);
    REQUIRE(empty.Empty());
}

TEST_CASE("RangeSet::Subtract matches a per-address model", "[range_sets]") {
    std::mt19937_64 rng{0x0400486024DEC000ULL};
    Common::RangeSet<u64> set;
    std::array<bool, SPACE> model{};
    for (int step = 0; step < 20000; ++step) {
        const u64 base = rng() % SPACE;
        const u64 size = rng() % (SPACE - base + 1);
        const bool add = rng() % 3 == 0;
        if (add) {
            set.Add(base, size);
        } else {
            set.Subtract(base, size);
        }
        for (u64 address = base; address < base + size; ++address) {
            model[address] = add;
        }
        REQUIRE(Contents(set) == model);
    }
}

TEST_CASE("RangeSet stays sorted and joined, and its lookups match a per-address model",
          "[range_sets]") {
    std::mt19937_64 rng{0x0400486024DEC001ULL};
    Common::RangeSet<u64> set;
    std::array<bool, SPACE> model{};
    for (int step = 0; step < 20000; ++step) {
        // Mostly small ranges, so that the set holds many.
        const u64 base = rng() % SPACE;
        const u64 limit = rng() % 8 == 0 ? SPACE - base : (std::min<u64>)(16, SPACE - base);
        const u64 size = rng() % (limit + 1);
        const bool add = rng() % 2 == 0;
        if (add) {
            set.Add(base, size);
        } else {
            set.Subtract(base, size);
        }
        for (u64 address = base; address < base + size; ++address) {
            model[address] = add;
        }

        u64 previous_end = 0;
        bool first = true;
        set.ForEach([&](u64 begin, u64 end) {
            REQUIRE(begin < end);
            // Touching ranges would have joined.
            REQUIRE((first || previous_end < begin));
            previous_end = end;
            first = false;
        });
        REQUIRE(set.Empty() == first);

        const u64 query = rng() % SPACE;
        const u64 query_size = rng() % (SPACE - query + 1);
        bool expected_hit = false;
        std::array<bool, SPACE> expected{};
        for (u64 address = query; address < query + query_size; ++address) {
            expected[address] = model[address];
            expected_hit = expected_hit || model[address];
        }
        REQUIRE(set.Intersects(query, query_size) == expected_hit);
        std::array<bool, SPACE> visited{};
        set.ForEachInRange(query, query_size, [&](u64 begin, u64 end) {
            REQUIRE(begin >= query);
            REQUIRE(end <= query + query_size);
            REQUIRE(begin < end);
            for (u64 address = begin; address < end; ++address) {
                visited[address] = true;
            }
        });
        REQUIRE(visited == expected);
    }
}

TEST_CASE("RangeSet joins touching ranges and splits on a cut", "[range_sets]") {
    const auto count = [](const Common::RangeSet<u64>& set) {
        int ranges = 0;
        set.ForEach([&](u64, u64) { ++ranges; });
        return ranges;
    };
    Common::RangeSet<u64> set;
    set.Add(10, 10);
    set.Add(30, 10);
    REQUIRE(count(set) == 2);
    set.Add(20, 10); // Touches both.
    REQUIRE(count(set) == 1);
    set.Subtract(20, 5);
    REQUIRE(count(set) == 2);
    set.Add(5, 0);
    set.Add(50, 0);
    REQUIRE(count(set) == 2);
    set.Subtract(0, 100);
    REQUIRE(set.Empty());
}
