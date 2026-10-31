// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

namespace {
constexpr u64 Page = u64{1} << Tegra::Nx2WrittenPageBits;
constexpr u64 AddressSpace = u64{1} << 36;
} // Anonymous namespace

TEST_CASE("GPU-written command pages are tracked per page over the whole range",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(16 * Page + Page - 8, 16);
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(15 * Page + Page - 1));
    REQUIRE(Tegra::Nx2IsGpuWritten(16 * Page));
    REQUIRE(Tegra::Nx2IsGpuWritten(16 * Page + Page - 1));
    REQUIRE(Tegra::Nx2IsGpuWritten(17 * Page));
    REQUIRE(Tegra::Nx2IsGpuWritten(17 * Page + 7));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(18 * Page));
}

TEST_CASE("Empty and out of range writes mark nothing past the device address space",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(4 * Page, 0);
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(4 * Page));

    Tegra::Nx2MarkGpuWritten(AddressSpace - 4, 8);
    REQUIRE(Tegra::Nx2IsGpuWritten(AddressSpace - 1));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(AddressSpace));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(AddressSpace + Page));
}

TEST_CASE("A new session forgets the pages the previous one wrote",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(64 * Page, 3 * Page);
    REQUIRE(Tegra::Nx2IsGpuWritten(66 * Page));
    Tegra::Nx2ClearGpuWrittenPages();
    for (u64 page = 63; page <= 67; ++page) {
        REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(page * Page));
    }
}
