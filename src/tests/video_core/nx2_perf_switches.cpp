// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_diag.h"
#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf pending_wake is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::PendingWake)] == "pending_wake");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::PendingWake)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(true, "pending_wake", ""), Index(Opt::PendingWake)));
    REQUIRE(ParseOffMask(false, "", "pending_wake") == Bit(Opt::PendingWake));
}

TEST_CASE("Nx2Perf diag_draw_lite is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::DiagDrawLite)] == "diag_draw_lite");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::DiagDrawLite)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(!IsOff(ParseOffMask(true, "diag_draw_lite", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(ParseOffMask(false, "", "diag_draw_lite") == Bit(Opt::DiagDrawLite));
}

TEST_CASE("diag_draw_lite: folded draws equal the synchronous Touch and Note calls", "[nx2_perf]") {
    using namespace VideoCommon::Nx2Diag;
    Diagnostics sync{Diagnostics::Quiet{}, true};
    Diagnostics lite{Diagnostics::Quiet{}, true};
    for (Diagnostics* diag : {&sync, &lite}) {
        diag->RegisterProgram(0x1000, 0xabc);
        diag->RegisterProgram(0x3000, 0xabc);
    }
    const auto table = std::make_unique<LiteTable>();
    static constexpr std::array<const char*, 3> outcomes{"drawn", "async-build", "ablated"};
    std::mt19937_64 rng(7);
    for (int i = 0; i < 6000; ++i) {
        if (i == 3000) {
            // A key registered mid-run: the pending draws keep the keys of their generation.
            sync.RegisterProgram(0x2000, 0xdef);
            lite.RegisterProgram(0x2000, 0xdef);
        }
        const u64 vs = 0x1000 + (rng() % 4) * 0x1000;
        const u64 ps = 0x8000 + (rng() % 300) * 0x100;
        const char* const outcome = outcomes[rng() % outcomes.size()];
        const u64 instances = 1 + rng() % 3;
        const u64 vertices = rng() % 100;
        const u32 width = 640 + static_cast<u32>(rng() % 2);
        sync.Touch('v', vs, outcome, instances, vertices, width, 360);
        sync.Touch('p', ps, outcome, instances, vertices, width, 360);
        sync.Note(false, vs, ps, outcome, instances, vertices);
        const LiteAnswer answer = lite.LiteLookup(*table, vs, ps);
        const u64 expected_key = vs == 0x1000 || vs == 0x3000   ? u64{0xabc}
                                 : vs == 0x2000 && i >= 3000 ? u64{0xdef}
                                                             : u64{0};
        REQUIRE(answer.vkey == expected_key);
        lite.LiteAdd(*table, answer, outcome, instances, vertices, width, 360);
    }
    lite.FoldLite(*table);
    REQUIRE(table->used == 0);
    u32 lines = 0;
    REQUIRE(lite.DrawDifferences(sync, lines) == 0);
    sync.Touch('v', 0x5000, "drawn", 1, 3, 640, 360);
    REQUIRE(lite.DrawDifferences(sync, lines) != 0);
}
