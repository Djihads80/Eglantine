// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/slot_vector.h"

TEST_CASE("SlotVector visits live entries across empty bitmap words", "[common]") {
    Common::SlotVector<u32> slots;
    std::vector<Common::SlotId> ids;
    for (u32 i = 0; i < 513; ++i) {
        ids.push_back(slots.insert(i));
    }
    for (u32 i = 0; i < 512; ++i) {
        slots.erase(ids[i]);
    }
    auto it = slots.begin();
    REQUIRE(it != slots.end());
    REQUIRE((*it).first == ids[512]);
    REQUIRE(*(*it).second == 512);
    REQUIRE(++it == slots.end());
    slots.erase(ids[512]);
    REQUIRE(slots.begin() == slots.end());
}

TEST_CASE("SlotVector sparse iteration matches a live-entry model", "[common]") {
    Common::SlotVector<u32> slots;
    std::map<u32, u32> live;
    std::mt19937 random{9813};
    for (u32 batch = 0; batch < 200; ++batch) {
        for (u32 i = 0; i < 100; ++i) {
            if (!live.empty() && (random() % 3) == 0) {
                auto selected = live.begin();
                std::advance(selected, random() % live.size());
                slots.erase(Common::SlotId{selected->first});
                live.erase(selected);
            } else {
                const u32 value = random();
                const auto id = slots.insert(value);
                live.emplace(id.Value(), value);
            }
        }
        auto expected = live.begin();
        for (const auto [id, value] : slots) {
            REQUIRE(expected != live.end());
            REQUIRE(id.Value() == expected->first);
            REQUIRE(*value == expected->second);
            ++expected;
        }
        REQUIRE(expected == live.end());
    }
}
