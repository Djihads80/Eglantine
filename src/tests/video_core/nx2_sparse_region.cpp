// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_sparse_region.h"

using Service::Nx2SparseRegion;

TEST_CASE("Nx2 sparse backing keeps its original block extent", "[nx2_mapping]") {
    Nx2SparseRegion region{Nx2SparseRegion::BlockSize + 0x1000, 1};
    const auto first = region.BeginBacking(0);
    REQUIRE(first);
    CHECK(first->size == Nx2SparseRegion::BlockSize);
    const auto last = region.BeginBacking(1);
    REQUIRE(last);
    CHECK(last->size == 0x1000);
    CHECK_FALSE(region.BeginBacking(2));
    CHECK_FALSE(region.BeginBacking(0));
    CHECK(region.IsPending(*first));
    CHECK(region.FinishBacking(*first, true));
    CHECK(region.blocks[0] == Nx2SparseRegion::Block::Backed);
    CHECK_FALSE(region.IsPending(*first));
    CHECK_FALSE(region.FinishBacking(*first, false));
    CHECK_FALSE(region.BeginBacking(0));
}

TEST_CASE("Nx2 sparse allocation failure resolves pending work without retrying", "[nx2_mapping]") {
    Nx2SparseRegion region{Nx2SparseRegion::BlockSize, 1};
    const auto pending = region.BeginBacking(0);
    REQUIRE(pending);
    CHECK(region.FinishBacking(*pending, false));
    CHECK_FALSE(region.IsPending(*pending));
    CHECK(region.blocks[0] == Nx2SparseRegion::Block::Failed);
    CHECK_FALSE(region.BeginBacking(0));
    CHECK_FALSE(region.FinishBacking(*pending, true));
}

TEST_CASE("Nx2 destroyed sparse regions cannot accept stale backing", "[nx2_mapping]") {
    std::map<u64, Nx2SparseRegion> regions;
    constexpr u64 Address = 0x100000;
    regions.emplace(Address, Nx2SparseRegion{Nx2SparseRegion::BlockSize, 1});
    const auto old = regions.at(Address).BeginBacking(0);
    REQUIRE(old);
    regions.erase(Address);
    CHECK(regions.find(Address) == regions.end());
    regions.emplace(Address, Nx2SparseRegion{0x1000, 2});
    auto& replacement = regions.at(Address);
    CHECK_FALSE(replacement.FinishBacking(*old, true));
    const auto current = replacement.BeginBacking(0);
    REQUIRE(current);
    CHECK_FALSE(replacement.IsPending(*old));
    CHECK_FALSE(replacement.FinishBacking(*old, false));
    CHECK(replacement.IsPending(*current));
    CHECK(current->size == 0x1000);
    CHECK(replacement.FinishBacking(*current, true));
    CHECK(replacement.blocks[0] == Nx2SparseRegion::Block::Backed);
}

TEST_CASE("Nx2 sparse backing rejects pending indices outside a replacement", "[nx2_mapping]") {
    Nx2SparseRegion old{2 * Nx2SparseRegion::BlockSize, 1};
    const auto pending = old.BeginBacking(1);
    REQUIRE(pending);
    Nx2SparseRegion replacement{Nx2SparseRegion::BlockSize, 2};
    CHECK_FALSE(replacement.IsPending(*pending));
    CHECK_FALSE(replacement.FinishBacking(*pending, true));
    CHECK(replacement.blocks[0] == Nx2SparseRegion::Block::Empty);
}
