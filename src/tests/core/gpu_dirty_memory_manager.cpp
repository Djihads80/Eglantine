// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/gpu_dirty_memory_manager.h"

namespace {
std::vector<std::pair<PAddr, size_t>> GatherAll(Core::GPUDirtyMemoryManager& manager) {
    std::vector<std::pair<PAddr, size_t>> runs;
    std::function<void(PAddr, size_t)> callback{
        [&runs](PAddr address, size_t size) { runs.emplace_back(address, size); }};
    manager.Gather(callback);
    return runs;
}
} // namespace

TEST_CASE("GPUDirtyMemoryManager gathers nothing until a write and every write after one",
          "[core][gpu_dirty_memory]") {
    Core::GPUDirtyMemoryManager manager;
    // Nothing collected: the fast path reports nothing, and repeatedly.
    CHECK(GatherAll(manager).empty());
    CHECK(GatherAll(manager).empty());

    // One write: reported once, in 64-byte granules.
    manager.Collect(0x10000, 4);
    auto runs = GatherAll(manager);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].first == 0x10000);
    CHECK(runs[0].second == 64);
    CHECK(GatherAll(manager).empty());

    // Writes to different pages: the earlier page moves to the back buffer under the lock and the
    // newest stays current; a gather must still return both.
    manager.Collect(0x20000, 128);
    manager.Collect(0x40040, 8);
    runs = GatherAll(manager);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0] == std::pair<PAddr, size_t>{0x20000, 128});
    CHECK(runs[1] == std::pair<PAddr, size_t>{0x40040, 64});
    CHECK(GatherAll(manager).empty());
}
