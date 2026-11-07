// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf spin_handoff is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::SpinHandoff) == 70);
    REQUIRE(opt_names[Index(Opt::SpinHandoff)] == "spin_handoff");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(always_off, Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::SpinHandoff)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::SpinHandoff)));
    REQUIRE(ParseOffMask(false, "spin_handoff", "", false) ==
            (always_off & ~Bit(Opt::SpinHandoff)));
    REQUIRE(ParseOffMask(false, "spin_handoff", "spin_handoff", false) == always_off);
}

TEST_CASE("Nx2Perf run_ahead_cow_probe is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::RunAheadCowProbe) == 72);
    REQUIRE(opt_names[Index(Opt::RunAheadCowProbe)] == "run_ahead_cow_probe");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(always_off, Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::RunAheadCowProbe)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::RunAheadCowProbe)));
    REQUIRE(ParseOffMask(false, "run_ahead_cow_probe", "", false) ==
            (always_off & ~Bit(Opt::RunAheadCowProbe)));
    REQUIRE(ParseOffMask(false,
                         "frames_ahead,present_one_ahead,run_ahead_ledger,run_ahead_cow_probe",
                         "", false) ==
            (always_off & ~(Bit(Opt::FramesAhead) | Bit(Opt::PresentOneAhead) |
                            Bit(Opt::RunAheadLedger) | Bit(Opt::RunAheadCowProbe))));
    REQUIRE(ParseOffMask(false, "run_ahead_cow_probe", "run_ahead_cow_probe", false) ==
            always_off);
}
