// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf default mask keeps every switch on", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "") == u64{0});
}

TEST_CASE("Nx2Perf bank_once_per_frame is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::BankOncePerFrame)] == "bank_once_per_frame");
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::BankOncePerFrame)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::BankOncePerFrame)));
    REQUIRE(ParseOffMask(false, "", "bank_once_per_frame") == Bit(Opt::BankOncePerFrame));
}

TEST_CASE("Nx2Perf OFF=bind_memo flips only its bit", "[nx2_perf]") {
    const auto mask = ParseOffMask(false, "", "bind_memo");
    REQUIRE(mask == Bit(Opt::BindMemo));
    REQUIRE(!IsOff(mask, Index(Opt::IndexRebind)));
}

TEST_CASE("Nx2Perf SAFE is the default-off set", "[nx2_perf]") {
    const auto mask = ParseOffMask(true, "", "");
    REQUIRE(mask == DefaultOffMask());
    for (size_t i = Index(Opt::Bounds); i <= Index(Opt::IndexRebind); ++i) {
        REQUIRE(!IsOff(mask, i));
    }
    for (size_t i = Index(Opt::DownloadBatch); i <= Index(Opt::BindMemo); ++i) {
        REQUIRE(IsOff(mask, i));
    }
}

TEST_CASE("Nx2Perf lists of names", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "bounds,draw_preamble") ==
            (Bit(Opt::Bounds) | Bit(Opt::DrawPreamble)));
    REQUIRE(ParseOffMask(true, "front_run,download_batch", "") ==
            (DefaultOffMask() & ~(Bit(Opt::FrontRun) | Bit(Opt::DownloadBatch))));
}

TEST_CASE("compute_rebuild_async: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ComputeRebuildAsync)] == "compute_rebuild_async");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::ComputeRebuildAsync)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::ComputeRebuildAsync)));
    REQUIRE(ParseOffMask(false, "", "compute_rebuild_async") == Bit(Opt::ComputeRebuildAsync));
}

TEST_CASE("compute_preload_variants: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ComputePreloadVariants)] == "compute_preload_variants");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::ComputePreloadVariants)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::ComputePreloadVariants)));
    REQUIRE(ParseOffMask(false, "", "compute_preload_variants") ==
            Bit(Opt::ComputePreloadVariants));
}

TEST_CASE("stale_target_skip: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::StaleTargetSkip)] == "stale_target_skip");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::StaleTargetSkip)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::StaleTargetSkip)));
    REQUIRE(ParseOffMask(false, "", "stale_target_skip") == Bit(Opt::StaleTargetSkip));
}

TEST_CASE("geometry_consistency and build_order_priority: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::GeometryConsistency)] == "geometry_consistency");
    REQUIRE(opt_names[Index(Opt::BuildOrderPriority)] == "build_order_priority");
    REQUIRE(opt_names[Index(Opt::ReplayDiskRecordings)] == "replay_disk_recordings");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::GeometryConsistency)));
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::BuildOrderPriority)));
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::ReplayDiskRecordings)));
}
