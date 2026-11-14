// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_layout.h"

namespace {
constexpr u64 Allowed = 0xffffffff;
constexpr std::array<Common::CpuCacheDomain, 2> Caches{{{0xffff, 96}, {0xffff0000, 32}}};

std::vector<u64> CoreMasks() {
    std::vector<u64> cores;
    for (u32 i = 0; i < 16; ++i) {
        cores.push_back(u64{3} << (2 * i));
    }
    return cores;
}
} // namespace

TEST_CASE("CPU layout preserves complete cores and partitions allowed processors", "[cpu_layout]") {
    const auto cores = CoreMasks();
    const auto layout = Common::MakeCpuThreadLayout(Allowed, Caches, cores);
    REQUIRE(layout);
    CHECK(layout->foreground == 0x3fff);
    CHECK(layout->backend == 0xc000);
    CHECK(layout->support == 0xffff0000);
    CHECK(layout->front == 0x3000);
    CHECK((layout->front & ~layout->foreground) == 0);
    CHECK((layout->foreground | layout->backend | layout->support) == Allowed);
    CHECK((layout->foreground & layout->backend) == 0);
    CHECK((layout->support & (layout->foreground | layout->backend)) == 0);
    CHECK(std::count(cores.begin(), cores.end(), layout->backend) == 1);
}

TEST_CASE("CPU layout selection is independent of topology enumeration order", "[cpu_layout]") {
    auto cores = CoreMasks();
    auto caches = Caches;
    std::reverse(cores.begin(), cores.end());
    std::reverse(caches.begin(), caches.end());
    const auto layout = Common::MakeCpuThreadLayout(Allowed, caches, cores);
    REQUIRE(layout);
    CHECK(layout->backend == 0xc000);
    CHECK(layout->foreground == 0x3fff);
    CHECK(layout->support == 0xffff0000);
}

TEST_CASE("CPU layout rejects unsupported domains and partial affinity", "[cpu_layout]") {
    const auto cores = CoreMasks();
    CHECK_FALSE(Common::MakeCpuThreadLayout(0xffff, Caches, cores));
    CHECK_FALSE(Common::MakeCpuThreadLayout(0, Caches, cores));
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, {}, cores));
    auto caches = Caches;
    caches[1].bytes = caches[0].bytes;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, caches, cores));
    caches = Caches;
    caches[0].bytes = 0;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, caches, cores));
    caches = Caches;
    caches[0].mask |= 0x10000;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, caches, cores));
    caches = Caches;
    caches[0].mask &= ~u64{1};
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, caches, cores));
}

TEST_CASE("CPU layout rejects incomplete overlapping and cross-domain physical cores",
          "[cpu_layout]") {
    auto cores = CoreMasks();
    cores.pop_back();
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
    cores = CoreMasks();
    cores.push_back(cores[0]);
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
    cores = CoreMasks();
    cores[0] = 0;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
    cores = CoreMasks();
    cores[0] |= u64{1} << 32;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
    cores = CoreMasks();
    std::swap(cores[0], cores[8]);
    cores[0] = 0x10001;
    cores[8] = 0x20002;
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
    cores = CoreMasks();
    cores[0] |= cores[1];
    cores.erase(cores.begin() + 1);
    CHECK_FALSE(Common::MakeCpuThreadLayout(Allowed, Caches, cores));
}
