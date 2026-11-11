// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "common/range_map.h"

enum class MappedEnum : u32 {
    Invalid = 0,
    Valid_1 = 1,
    Valid_2 = 2,
    Valid_3 = 3,
};

TEST_CASE("Range Map: Cached sizes survive remaps, splits and merges", "[video_core]") {
    Common::RangeMap<u64, MappedEnum> map(MappedEnum::Invalid);
    std::array<MappedEnum, 4096> reference{};
    std::mt19937 random{0x1bf12000};
    for (u32 operation = 0; operation < 20000; ++operation) {
        const u32 begin = random() % reference.size();
        const u32 end = begin + 1 + random() % (reference.size() - begin);
        const auto value = static_cast<MappedEnum>(random() % 4);
        if (value == MappedEnum::Invalid) {
            map.Unmap(begin, end);
        } else {
            map.Map(begin, end, value);
        }
        std::fill(reference.begin() + begin, reference.begin() + end, value);
        for (const u32 address :
             {begin, end - 1, end, static_cast<u32>(random() % reference.size())}) {
            u32 expected{};
            if (address < reference.size() && reference[address] != MappedEnum::Invalid) {
                while (address + expected < reference.size() &&
                       reference[address + expected] == reference[address]) {
                    ++expected;
                }
            }
            REQUIRE(map.GetContinuousSizeFrom(address) == expected);
            REQUIRE(map.GetContinuousSizeFrom(address, true) == expected);
            REQUIRE(map.GetContinuousSizeFrom(address, true) == expected);
            if (expected > 1) {
                REQUIRE(map.GetContinuousSizeFrom(address + 1, true) == expected - 1);
                REQUIRE(map.GetContinuousSizeFrom(address + expected - 1, true) == 1);
            }
        }
    }
    map.Unmap(0, reference.size());
    map.Map(1, 100, MappedEnum::Valid_1);
    REQUIRE(map.GetContinuousSizeFrom(1, true) == 99);
    REQUIRE(map.GetContinuousSizeFrom(~u64{0}, true) == 0);
    REQUIRE(map.GetContinuousSizeFrom(1, true) == 99);
    map.Unmap(0, reference.size());
    REQUIRE(map.GetContinuousSizeFrom(1, true) == 0);
}

TEST_CASE("Range Map: cache hits retain the same guarded mapping lifetime", "[video_core]") {
    Common::RangeMap<u64, MappedEnum> map(MappedEnum::Invalid);
    map.Map(100, 200, MappedEnum::Valid_1);
    bool hit{};
    REQUIRE(map.GetContinuousSizeFrom(110, true, &hit) == 90);
    REQUIRE_FALSE(hit);
    REQUIRE(map.GetContinuousSizeFrom(120, true, &hit) == 80);
    REQUIRE(hit);
    REQUIRE(map.GetContinuousSizeFrom(120, false, &hit) == 80);
    REQUIRE_FALSE(hit);
    map.Map(150, 200, MappedEnum::Valid_2);
    REQUIRE(map.GetContinuousSizeFrom(120, true, &hit) == 30);
    REQUIRE_FALSE(hit);
    REQUIRE(map.GetContinuousSizeFrom(140, true, &hit) == 10);
    REQUIRE(hit);
    map.Unmap(130, 160);
    REQUIRE(map.GetContinuousSizeFrom(140, true, &hit) == 0);
    REQUIRE_FALSE(hit);
    REQUIRE(map.GetContinuousSizeFrom(129, true, &hit) == 1);
    REQUIRE_FALSE(hit);
}

TEST_CASE("Range Map: Setup", "[video_core]") {
    Common::RangeMap<u64, MappedEnum> my_map(MappedEnum::Invalid);
    my_map.Map(3000, 3500, MappedEnum::Valid_1);
    my_map.Unmap(3200, 3600);
    my_map.Map(4000, 4500, MappedEnum::Valid_2);
    my_map.Map(4200, 4400, MappedEnum::Valid_2);
    my_map.Map(4200, 4400, MappedEnum::Valid_1);
    REQUIRE(my_map.GetContinuousSizeFrom(4200) == 200);
    REQUIRE(my_map.GetContinuousSizeFrom(3000) == 200);
    REQUIRE(my_map.GetContinuousSizeFrom(2900) == 0);

    REQUIRE(my_map.GetValueAt(2900) == MappedEnum::Invalid);
    REQUIRE(my_map.GetValueAt(3100) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(3000) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(3200) == MappedEnum::Invalid);

    REQUIRE(my_map.GetValueAt(4199) == MappedEnum::Valid_2);
    REQUIRE(my_map.GetValueAt(4200) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(4400) == MappedEnum::Valid_2);
    REQUIRE(my_map.GetValueAt(4500) == MappedEnum::Invalid);
    REQUIRE(my_map.GetValueAt(4600) == MappedEnum::Invalid);

    my_map.Unmap(0, 6000);
    for (u64 address = 0; address < 10000; address += 1000) {
        REQUIRE(my_map.GetContinuousSizeFrom(address) == 0);
    }

    my_map.Map(1000, 3000, MappedEnum::Valid_1);
    my_map.Map(4000, 5000, MappedEnum::Valid_1);
    my_map.Map(2500, 4100, MappedEnum::Valid_1);
    REQUIRE(my_map.GetContinuousSizeFrom(1000) == 4000);

    my_map.Map(1000, 3000, MappedEnum::Valid_1);
    my_map.Map(4000, 5000, MappedEnum::Valid_2);
    my_map.Map(2500, 4100, MappedEnum::Valid_3);
    REQUIRE(my_map.GetContinuousSizeFrom(1000) == 1500);
    REQUIRE(my_map.GetContinuousSizeFrom(2500) == 1600);
    REQUIRE(my_map.GetContinuousSizeFrom(4100) == 900);
    REQUIRE(my_map.GetValueAt(900) == MappedEnum::Invalid);
    REQUIRE(my_map.GetValueAt(1000) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(2500) == MappedEnum::Valid_3);
    REQUIRE(my_map.GetValueAt(4100) == MappedEnum::Valid_2);
    REQUIRE(my_map.GetValueAt(5000) == MappedEnum::Invalid);

    my_map.Map(2000, 6000, MappedEnum::Valid_3);
    REQUIRE(my_map.GetContinuousSizeFrom(1000) == 1000);
    REQUIRE(my_map.GetContinuousSizeFrom(3000) == 3000);
    REQUIRE(my_map.GetValueAt(1000) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(1999) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(1500) == MappedEnum::Valid_1);
    REQUIRE(my_map.GetValueAt(2001) == MappedEnum::Valid_3);
    REQUIRE(my_map.GetValueAt(5999) == MappedEnum::Valid_3);
    REQUIRE(my_map.GetValueAt(6000) == MappedEnum::Invalid);
}
