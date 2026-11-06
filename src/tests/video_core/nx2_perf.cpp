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

TEST_CASE("Nx2Perf default mask turns off only the always-off switches", "[nx2_perf]") {
    const u64 mask = ParseOffMask(false, "", "", false);
    REQUIRE(mask == always_off);
    for (size_t i = 0; i < opt_names.size(); ++i) {
        const bool expected_off = i == Index(Opt::DispatchBatch) || i == Index(Opt::AcquireEarly) ||
                                  i == Index(Opt::SplitOverlap) ||
                                  i == Index(Opt::MacroReadOverlayWriters) ||
                                  i == Index(Opt::GpuCorePin) ||
                                  i == Index(Opt::PresentGuestWait) ||
                                  i == Index(Opt::PresentOneAhead) ||
                                  i == Index(Opt::ReleaseBeforeHook) ||
                                  i == Index(Opt::AcquireTriggerEarly) ||
                                  i == Index(Opt::PipelineKeepPreloaded) ||
                                  i == Index(Opt::WriterSetExec) ||
                                  i == Index(Opt::ExactNotedWrites) ||
                                  i == Index(Opt::GuestCcd) ||
                                  i == Index(Opt::LandedPrevAlways) ||
                                  i == Index(Opt::OneAheadLanded);
        INFO(opt_names[i]);
        REQUIRE(IsOff(mask, i) == expected_off);
    }
}

TEST_CASE("Nx2Perf exact_noted_writes is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::ExactNotedWrites) == 46);
    REQUIRE(opt_names[Index(Opt::ExactNotedWrites)] == "exact_noted_writes");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::ExactNotedWrites)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::ExactNotedWrites)));
    REQUIRE(ParseOffMask(false, "exact_noted_writes", "", false) ==
            (always_off & ~Bit(Opt::ExactNotedWrites)));
}

TEST_CASE("Nx2Perf bank_once_per_frame is on by default and off under SAFE", "[nx2_perf]") {
    static_assert(Index(Opt::BankOncePerFrame) == 48);
    REQUIRE(opt_names[Index(Opt::BankOncePerFrame)] == "bank_once_per_frame");
    REQUIRE(!IsOff(ParseOffMask(false, "", "", false), Index(Opt::BankOncePerFrame)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::BankOncePerFrame)));
    REQUIRE(ParseOffMask(false, "", "bank_once_per_frame", false) ==
            (always_off | Bit(Opt::BankOncePerFrame)));
}

TEST_CASE("Nx2Perf ON overrides always_off for the landed switches", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::LandedPrevAlways)] == "landed_prev_always");
    REQUIRE(opt_names[Index(Opt::OneAheadLanded)] == "one_ahead_landed");
    REQUIRE(ParseOffMask(false, "present_one_ahead,one_ahead_landed", "", false) ==
            (always_off & ~Bit(Opt::PresentOneAhead) & ~Bit(Opt::OneAheadLanded)));
    REQUIRE(ParseOffMask(false, "landed_prev_always", "", false) ==
            (always_off & ~Bit(Opt::LandedPrevAlways)));
}

TEST_CASE("Nx2Perf OFF=bind_memo flips only bit 36", "[nx2_perf]") {
    const u64 mask = ParseOffMask(false, "", "bind_memo", false);
    REQUIRE(mask == (always_off | (u64{1} << 36)));
    REQUIRE(!IsOff(mask, Index(Opt::IndexRebind)));
}

TEST_CASE("Nx2Perf OFF=dispatch_batch keeps method on", "[nx2_perf]") {
    const u64 mask = ParseOffMask(false, "", "dispatch_batch", false);
    REQUIRE(IsOff(mask, Index(Opt::DispatchBatch)));
    REQUIRE(!IsOff(mask, Index(Opt::Method)));
    REQUIRE(mask == always_off);

    const u64 on_then_off = ParseOffMask(false, "dispatch_batch", "dispatch_batch", false);
    REQUIRE(on_then_off == always_off);
}

TEST_CASE("Nx2Perf SAFE is the default-off set plus the always-off set", "[nx2_perf]") {
    const u64 mask = ParseOffMask(true, "", "", false);
    REQUIRE(mask == (DefaultOffMask() | always_off));
    for (size_t i = Index(Opt::Bounds); i <= Index(Opt::IndexRebind); ++i) {
        REQUIRE(!IsOff(mask, i));
    }
    for (size_t i = Index(Opt::DownloadBatch); i <= Index(Opt::BindMemo); ++i) {
        REQUIRE(IsOff(mask, i));
    }
}

TEST_CASE("Nx2Perf ON=dispatch_batch clears bit 33", "[nx2_perf]") {
    const u64 mask = ParseOffMask(false, "dispatch_batch", "", false);
    REQUIRE(!IsOff(mask, 33));
    REQUIRE(mask == (always_off & ~(u64{1} << 33)));
}

TEST_CASE("Nx2Perf FRAMES_AHEAD=0 and lists of names", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "", true) == (always_off | (u64{1} << Index(Opt::FramesAhead))));
    const u64 mask = ParseOffMask(false, "acquire_early,dispatch_batch", "bounds,draw_preamble", false);
    REQUIRE(mask == ((always_off & ~(Bit(Opt::AcquireEarly) | Bit(Opt::DispatchBatch))) |
                     Bit(Opt::Bounds) | Bit(Opt::DrawPreamble)));
}
