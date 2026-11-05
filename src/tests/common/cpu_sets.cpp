// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/windows/cpu_sets.h"

namespace {
using Common::Windows::CpuSet;
using Common::Windows::SelectPerformanceCpuSets;

TEST_CASE("Performance CPU Sets use class and IDs rather than processor order",
          "[common][cpu_sets]") {
    const std::array topology{
        CpuSet{300, 0, 1, 0, false}, CpuSet{901, 0, 22, 2, false},
        CpuSet{17, 0, 10, 1, false}, CpuSet{802, 0, 0, 2, false},
    };
    CHECK(SelectPerformanceCpuSets(topology, 0, ~u64{0}) ==
          std::vector<u32>{901, 802});
    CHECK(SelectPerformanceCpuSets(topology, 0, (u64{1} << 1) | (u64{1} << 10)) ==
          std::vector<u32>{17});
    CHECK(SelectPerformanceCpuSets(topology, 0, u64{1} << 22).empty());
    CHECK(SelectPerformanceCpuSets(topology, 0, 0).empty());
}

TEST_CASE("Performance CPU Sets respect group reservations and process restrictions",
          "[common][cpu_sets]") {
    const std::array topology{
        CpuSet{10, 0, 0, 0, false}, CpuSet{11, 0, 1, 2, false},
        CpuSet{12, 0, 2, 3, true}, CpuSet{13, 1, 1, 4, false},
        CpuSet{14, 0, 64, 5, false}, CpuSet{15, 0, 63, 2, false},
    };
    CHECK(SelectPerformanceCpuSets(topology, 0, ~u64{0}) == std::vector<u32>{11, 15});
    const std::array<u32, 3> defaults{10, 12, 15};
    CHECK(SelectPerformanceCpuSets(topology, 0, ~u64{0}, defaults) == std::vector<u32>{15});
    const std::array<u32, 2> unavailable{12, 99};
    CHECK(SelectPerformanceCpuSets(topology, 0, ~u64{0}, unavailable).empty());
    CHECK(SelectPerformanceCpuSets(topology, 1, ~u64{0}).empty());
}

TEST_CASE("Performance CPU Sets leave homogeneous or empty topologies unchanged",
          "[common][cpu_sets]") {
    const std::array topology{CpuSet{100, 0, 0, 7, false}, CpuSet{101, 0, 1, 7, false}};
    CHECK(SelectPerformanceCpuSets(topology, 0, 3).empty());
    CHECK(SelectPerformanceCpuSets({}, 0, ~u64{0}).empty());
}
} // namespace
