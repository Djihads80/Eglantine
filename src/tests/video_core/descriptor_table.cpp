// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/textures/texture.h"

TEST_CASE("Descriptor tables grow without reserving large unused ranges", "[video_core][memory]") {
    VideoCommon::DescriptorTable<Tegra::Texture::TICEntry> table;
    constexpr std::array<u32, 7> limits{0, 1, 63, 64, 255, 1024, 0xfffff};

    for (const u32 limit : limits) {
        table.Refresh(GPUVAddr{0x1000}, limit);
        REQUIRE(table.descriptors.size() >= static_cast<size_t>(limit) + 1);
        REQUIRE(table.descriptors.size() <= (static_cast<size_t>(limit) + 1) * 2);
        REQUIRE(table.read_descriptors.size() * 64 >= static_cast<size_t>(limit) + 1);
        REQUIRE(std::ranges::all_of(table.read_descriptors, [](u64 bits) { return bits == 0; }));
        std::ranges::fill(table.read_descriptors, ~u64{0});
    }
}

TEST_CASE("Refreshing a descriptor table clears old read marks", "[video_core][memory]") {
    VideoCommon::DescriptorTable<Tegra::Texture::TICEntry> table;
    table.Refresh(GPUVAddr{0x1000}, 255);
    REQUIRE(table.descriptors.size() * sizeof(Tegra::Texture::TICEntry) <= 16 * 1024);
    std::ranges::fill(table.read_descriptors, ~u64{0});

    REQUIRE_FALSE(table.Synchronize(GPUVAddr{0x1000}, 255));
    REQUIRE(table.Synchronize(GPUVAddr{0x2000}, 127));
    REQUIRE(std::ranges::all_of(table.read_descriptors, [](u64 bits) { return bits == 0; }));
    std::ranges::fill(table.read_descriptors, ~u64{0});
    REQUIRE(table.Synchronize(GPUVAddr{0x2000}, 1024));
    REQUIRE(std::ranges::all_of(table.read_descriptors, [](u64 bits) { return bits == 0; }));
}
