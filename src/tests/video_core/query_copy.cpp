// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_query_copy.h"

TEST_CASE("Query copies retain the last write to a repeated destination", "[video_core]") {
    const std::array<VkBufferCopy, 2> copies{{{0, 69728, 8}, {8, 69728, 8}}};
    const auto result = Vulkan::NonOverlappingQueryCopies(copies);
    REQUIRE(result.size() == 1);
    CHECK(result[0].srcOffset == 8);
    CHECK(result[0].dstOffset == 69728);
    CHECK(result[0].size == 8);
}

TEST_CASE("Query copies preserve the source offsets of split writes", "[video_core]") {
    const std::array<VkBufferCopy, 2> copies{{{32, 0, 16}, {80, 4, 4}}};
    const auto result = Vulkan::NonOverlappingQueryCopies(copies);
    REQUIRE(result.size() == 3);
    CHECK(result[0].srcOffset == 32);
    CHECK(result[0].dstOffset == 0);
    CHECK(result[0].size == 4);
    CHECK(result[1].srcOffset == 80);
    CHECK(result[1].dstOffset == 4);
    CHECK(result[1].size == 4);
    CHECK(result[2].srcOffset == 40);
    CHECK(result[2].dstOffset == 8);
    CHECK(result[2].size == 8);
}

TEST_CASE("Nonoverlapping query copies match ordered writes byte for byte", "[video_core]") {
    std::minstd_rand random{1034};
    for (size_t batch = 0; batch < 300; ++batch) {
        std::array<VkDeviceSize, 64> expected{}, actual{};
        expected.fill(~VkDeviceSize{});
        actual.fill(~VkDeviceSize{});
        std::vector<VkBufferCopy> copies;
        for (size_t i = 0; i < batch % 32; ++i) {
            const VkDeviceSize destination = random() % expected.size();
            const VkDeviceSize size = random() % (expected.size() - destination + 1);
            const VkDeviceSize source = random() % 1024;
            copies.push_back({source, destination, size});
            for (VkDeviceSize byte = 0; byte < size; ++byte) {
                expected[destination + byte] = source + byte;
            }
        }
        const auto result = Vulkan::NonOverlappingQueryCopies(copies);
        VkDeviceSize previous_end = 0;
        for (const auto& copy : result) {
            CHECK(copy.size > 0);
            CHECK(copy.dstOffset >= previous_end);
            previous_end = copy.dstOffset + copy.size;
            for (VkDeviceSize byte = 0; byte < copy.size; ++byte) {
                actual[copy.dstOffset + byte] = copy.srcOffset + byte;
            }
        }
        CHECK(actual == expected);
    }
}
