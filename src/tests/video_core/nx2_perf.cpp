// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

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
                                  i == Index(Opt::FramesAhead) ||
                                  i == Index(Opt::ReleaseBeforeHook) ||
                                  i == Index(Opt::AcquireTriggerEarly) ||
                                  i == Index(Opt::PipelineKeepPreloaded) ||
                                  i == Index(Opt::WriterSetExec) ||
                                  i == Index(Opt::ExactNotedWrites) ||
                                  i == Index(Opt::GuestCcd) ||
                                  i == Index(Opt::LandedPrevAlways) ||
                                  i == Index(Opt::OneAheadLanded) ||
                                  i == Index(Opt::SplitRings) ||
                                  i == Index(Opt::BindMemoLocal) ||
                                  i == Index(Opt::GuestLandedSyncpoints) ||
                                  i == Index(Opt::WarGuardProbe) ||
                                  i == Index(Opt::HookLazyFlush) ||
                                  i == Index(Opt::FrontSyncTrio);
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

TEST_CASE("Nx2Perf front_sync_trio turns on its three switches", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::FrontSyncTrio)] == "front_sync_trio");
    const u64 trio = Bit(Opt::FrontSyncTrio) | front_sync_trio_set;
    REQUIRE(ParseOffMask(false, "front_sync_trio", "", false) == (always_off & ~trio));
    REQUIRE(ParseOffMask(true, "front_sync_trio", "", false) ==
            ((DefaultOffMask() | always_off) & ~trio));
    REQUIRE(ParseOffMask(false, "front_sync_trio", "writer_set_exec", false) ==
            ((always_off & ~trio) | Bit(Opt::WriterSetExec)));
    REQUIRE(ParseOffMask(false, "", "", false) == always_off);
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

TEST_CASE("Nx2Perf bind_memo_local is off unless ON names it", "[nx2_perf]") {
    static_assert(Index(Opt::BindMemoLocal) == 52);
    REQUIRE(opt_names[Index(Opt::BindMemoLocal)] == "bind_memo_local");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::BindMemoLocal)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::BindMemoLocal)));
    REQUIRE(ParseOffMask(false, "bind_memo_local", "", false) ==
            (always_off & ~Bit(Opt::BindMemoLocal)));
    REQUIRE(!IsOff(ParseOffMask(false, "bind_memo_local", "", false), Index(Opt::BindMemo)));
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

TEST_CASE("Nx2Perf live control: label, on and off lists", "[nx2_perf]") {
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult result =
        ParseControl(startup, "label B.3\non acquire_trigger_early,frames_ahead\noff cb_batch\n",
                     live_safe);
    REQUIRE(result.label == "B.3");
    REQUIRE(result.mask == ((startup & ~(Bit(Opt::AcquireTriggerEarly) | Bit(Opt::FramesAhead))) |
                            Bit(Opt::CbBatch)));
    REQUIRE(result.on_vs_startup == (Bit(Opt::AcquireTriggerEarly) | Bit(Opt::FramesAhead)));
    REQUIRE(result.off_vs_startup == Bit(Opt::CbBatch));
    REQUIRE(result.ignored.empty());
    REQUIRE(FormatControl(result, result.mask) ==
            fmt::format("set=B.3 on=frames_ahead,acquire_trigger_early off=cb_batch ignored=- "
                        "mask=0x{:016x}",
                        result.mask));
}

TEST_CASE("Nx2Perf live control: safe, comments and CRLF", "[nx2_perf]") {
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult result =
        ParseControl(startup, "\xEF\xBB\xBF# arm\r\n\r\n  label A # name\r\nsafe\r\n", live_safe);
    REQUIRE(result.label == "A");
    REQUIRE(result.mask == (startup | (live_safe & (DefaultOffMask() | always_off))));
    REQUIRE(result.off_vs_startup == (Bit(Opt::FrontRun) | Bit(Opt::DownloadBatch)));
    REQUIRE(result.on_vs_startup == 0);
    REQUIRE(result.ignored.empty());
    // In file order: a later "on" wins over "safe".
    REQUIRE(ParseControl(startup, "safe\non front_run", live_safe).mask ==
            (result.mask & ~Bit(Opt::FrontRun)));
}

TEST_CASE("Nx2Perf live control: switches not live-safe and unknown names are ignored",
          "[nx2_perf]") {
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult result =
        ParseControl(startup, "on present_one_ahead,bogus\noff bounds\nfaster\n", live_safe);
    REQUIRE(result.mask == startup);
    REQUIRE(result.on_vs_startup == 0);
    REQUIRE(result.off_vs_startup == 0);
    REQUIRE(result.ignored == "present_one_ahead,bogus,bounds,faster");
    REQUIRE(FormatControl(result, startup).starts_with(
        "set=- on=- off=- ignored=present_one_ahead,bogus,bounds,faster mask=0x"));
    // Nothing is live-safe with EDEN_NX2_PERF_OPT=0.
    REQUIRE(ParseControl(startup, "off cb_batch", 0).mask == startup);
}

TEST_CASE("Nx2Perf live control: each file is the whole arm", "[nx2_perf]") {
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult first = ParseControl(startup, "label A.1\noff clean_words\n", live_safe);
    REQUIRE(first.mask == (startup | Bit(Opt::CleanWords)));
    // The second file does not inherit the first one's "off".
    const ControlResult second = ParseControl(startup, "label B.2\noff front_sleep\n", live_safe);
    REQUIRE(second.mask == (startup | Bit(Opt::FrontSleep)));
    REQUIRE(second.off_vs_startup == Bit(Opt::FrontSleep));
    // An empty file is the startup mask.
    const ControlResult empty = ParseControl(startup, "", live_safe);
    REQUIRE(empty.mask == startup);
    REQUIRE(empty.label.empty());
    REQUIRE(FormatControl(empty, startup) ==
            fmt::format("set=- on=- off=- ignored=- mask=0x{:016x}", startup));
}

TEST_CASE("Nx2Perf live control: a label-only change still gets its own marker", "[nx2_perf]") {
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult a = ParseControl(startup, "label A.1\noff guest_ccd\n", live_safe);
    const ControlResult b = ParseControl(startup, "label A.2\noff guest_ccd\n", live_safe);
    REQUIRE(a.mask == b.mask);
    REQUIRE(a.ignored == "guest_ccd");
    REQUIRE(FormatControl(b, b.mask).starts_with("set=A.2 on=- off=- ignored=guest_ccd mask="));
    REQUIRE(FormatControl(a, a.mask) != FormatControl(b, b.mask));
}

TEST_CASE("Nx2Perf live-safe switches are all named", "[nx2_perf]") {
    REQUIRE((live_safe >> opt_names.size()) == 0);
    REQUIRE(!IsOff(live_safe, Index(Opt::PreflushAging)));
    // No On() call site: a live flip would change nothing, so it must land in ignored=.
    const u64 startup = ParseOffMask(false, "", "", false);
    const ControlResult dead = ParseControl(
        startup, "off bank_check,write_intersections,immediate_callbacks\n", live_safe);
    REQUIRE(dead.mask == startup);
    REQUIRE(dead.ignored == "bank_check,write_intersections,immediate_callbacks");
}
