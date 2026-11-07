// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/common_types.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_dma_forward.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_war_probe.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace VideoCommon {

struct GpuSplitLeanTestAccess {
    static void Initialize(GpuSplit& split, Core::System& system) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.publish_every = GpuSplit::RingWords;
    }
    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }
    static u64 Word(const GpuSplit& split, u64 position) {
        return split.ring[position & GpuSplit::RingMask];
    }
    /// One BackLoop step with back_replay_lean off (Execute) or on (ExecuteLean).
    static u64 Step(GpuSplit& split, bool lean, u64 position, u64 available, u64 last_store) {
        const size_t index = position & GpuSplit::RingMask;
        const size_t words = std::min<u64>(available - position, GpuSplit::RingWords - index);
        u64* const entry = &split.ring[index];
        const size_t size = lean ? split.ExecuteLean(entry, words, position, last_store)
                                 : split.Execute(entry, words);
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }
    static void SetRelease(GpuSplit& split, u64 target, bool hook) {
        split.wait_target.store(target, std::memory_order_relaxed);
        split.hook_requested.store(hook, std::memory_order_relaxed);
        split.idle_hook = [](void*, IdleHookReason) {};
    }
    static u64 PastTarget(const GpuSplit& split) {
        return split.lean_stats.past_target;
    }
    static u64 LeanUnits(const GpuSplit& split) {
        return split.lean_stats.units;
    }
    static u64 LeanCalls(const GpuSplit& split) {
        return split.lean_stats.calls;
    }
};

} // namespace VideoCommon

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf default mask turns off only the always-off switches", "[nx2_perf]") {
    const auto mask = ParseOffMask(false, "", "", false);
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
                                  i == Index(Opt::InflightWriteWait) ||
                                  i == Index(Opt::InflightReadExact) ||
                                  i == Index(Opt::FenceTimeline) ||
                                  i == Index(Opt::DmaForwardMemo) ||
                                  i == Index(Opt::RunAheadLedger) ||
                                  i == Index(Opt::ComputeWriterRanges) ||
                                  i == Index(Opt::AcquireQueuedRelease) ||
                                  i == Index(Opt::NonwriterProofMemo) ||
                                  i == Index(Opt::InflightWaitV2) ||
                                  i == Index(Opt::ListFetchAfterSync) ||
                                  i == Index(Opt::SpinHandoff) ||
                                  i == Index(Opt::RunAheadCover) ||
                                  i == Index(Opt::RunAheadCowProbe) ||
                                  i == Index(Opt::SyncFreeFetch) ||
                                  i == Index(Opt::BankHashShadow) ||
                                  i == Index(Opt::RunAheadRawProbe) ||
                                  i == Index(Opt::ListWriterHookCopiesOnly) ||
                                  i == Index(Opt::FrontMacroMemo) ||
                                  i == Index(Opt::RunAheadSnapshotProbe) ||
                                  i == Index(Opt::ReportSubmitEarly) ||
                                  i == Index(Opt::DmaForwardTape) ||
                                  i == Index(Opt::RunAheadOverlayBudget) ||
                                  i == Index(Opt::AcquireDrainLite) ||
                                  i == Index(Opt::ExecSameDrop) ||
                                  i == Index(Opt::StateDeadStore) ||
                                  i == Index(Opt::BackReplayLean);
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

TEST_CASE("Nx2Perf list_fetch_after_sync is off unless ON names it", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ListFetchAfterSync)] == "list_fetch_after_sync");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::ListFetchAfterSync)));
    REQUIRE(IsOff(always_off, Index(Opt::ListFetchAfterSync)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::ListFetchAfterSync)));
    REQUIRE(ParseOffMask(false, "list_fetch_after_sync", "", false) ==
            (always_off & ~Bit(Opt::ListFetchAfterSync)));
}

TEST_CASE("Nx2Perf front_sync_trio turns on its three switches", "[nx2_perf]") {
    const auto trio = front_sync_trio_set;
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
    const auto mask = ParseOffMask(false, "", "bind_memo", false);
    REQUIRE(mask == (always_off | Bit(Opt::BindMemo)));
    REQUIRE(!IsOff(mask, Index(Opt::IndexRebind)));
}

TEST_CASE("Nx2Perf OFF=dispatch_batch keeps method on", "[nx2_perf]") {
    const auto mask = ParseOffMask(false, "", "dispatch_batch", false);
    REQUIRE(IsOff(mask, Index(Opt::DispatchBatch)));
    REQUIRE(!IsOff(mask, Index(Opt::Method)));
    REQUIRE(mask == always_off);

    const auto on_then_off = ParseOffMask(false, "dispatch_batch", "dispatch_batch", false);
    REQUIRE(on_then_off == always_off);
}

TEST_CASE("Nx2Perf SAFE is the default-off set plus the always-off set", "[nx2_perf]") {
    const auto mask = ParseOffMask(true, "", "", false);
    REQUIRE(mask == (DefaultOffMask() | always_off));
    for (size_t i = Index(Opt::Bounds); i <= Index(Opt::IndexRebind); ++i) {
        REQUIRE(!IsOff(mask, i));
    }
    for (size_t i = Index(Opt::DownloadBatch); i <= Index(Opt::BindMemo); ++i) {
        REQUIRE(IsOff(mask, i));
    }
}

TEST_CASE("Nx2Perf ON=dispatch_batch clears bit 33", "[nx2_perf]") {
    const auto mask = ParseOffMask(false, "dispatch_batch", "", false);
    REQUIRE(!IsOff(mask, 33));
    REQUIRE(mask == (always_off & ~Bit(Opt::DispatchBatch)));
}

TEST_CASE("Nx2Perf FRAMES_AHEAD=0 and lists of names", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "", true) == (always_off | Bit(Opt::FramesAhead)));
    const auto mask = ParseOffMask(false, "acquire_early,dispatch_batch", "bounds,draw_preamble", false);
    REQUIRE(mask == ((always_off & ~(Bit(Opt::AcquireEarly) | Bit(Opt::DispatchBatch))) |
                     Bit(Opt::Bounds) | Bit(Opt::DrawPreamble)));
}

TEST_CASE("Nx2Perf live control: label, on and off lists", "[nx2_perf]") {
    const auto startup = ParseOffMask(false, "", "", false);
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
                        "mask=0x{:016x}{:016x}",
                        result.mask.hi, result.mask.lo));
}

TEST_CASE("Nx2Perf live control: safe, comments and CRLF", "[nx2_perf]") {
    const auto startup = ParseOffMask(false, "", "", false);
    const ControlResult result =
        ParseControl(startup, "\xEF\xBB\xBF# arm\r\n\r\n  label A # name\r\nsafe\r\n", live_safe);
    REQUIRE(result.label == "A");
    REQUIRE(result.mask == (startup | (live_safe & (DefaultOffMask() | always_off))));
    REQUIRE(result.off_vs_startup == (Bit(Opt::FrontRun) | Bit(Opt::DownloadBatch)));
    REQUIRE(result.on_vs_startup == OffMask{});
    REQUIRE(result.ignored.empty());
    // In file order: a later "on" wins over "safe".
    REQUIRE(ParseControl(startup, "safe\non front_run", live_safe).mask ==
            (result.mask & ~Bit(Opt::FrontRun)));
}

TEST_CASE("Nx2Perf live control: switches not live-safe and unknown names are ignored",
          "[nx2_perf]") {
    const auto startup = ParseOffMask(false, "", "", false);
    const ControlResult result =
        ParseControl(startup, "on present_one_ahead,bogus\noff bounds\nfaster\n", live_safe);
    REQUIRE(result.mask == startup);
    REQUIRE(result.on_vs_startup == OffMask{});
    REQUIRE(result.off_vs_startup == OffMask{});
    REQUIRE(result.ignored == "present_one_ahead,bogus,bounds,faster");
    REQUIRE(FormatControl(result, startup).starts_with(
        "set=- on=- off=- ignored=present_one_ahead,bogus,bounds,faster mask=0x"));
    // Nothing is live-safe with EDEN_NX2_PERF_OPT=0.
    REQUIRE(ParseControl(startup, "off cb_batch", OffMask{}).mask == startup);
}

TEST_CASE("Nx2Perf live control: each file is the whole arm", "[nx2_perf]") {
    const auto startup = ParseOffMask(false, "", "", false);
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
            fmt::format("set=- on=- off=- ignored=- mask=0x{:016x}{:016x}", startup.hi,
                        startup.lo));
}

TEST_CASE("Nx2Perf live control: a label-only change still gets its own marker", "[nx2_perf]") {
    const auto startup = ParseOffMask(false, "", "", false);
    const ControlResult a = ParseControl(startup, "label A.1\noff guest_ccd\n", live_safe);
    const ControlResult b = ParseControl(startup, "label A.2\noff guest_ccd\n", live_safe);
    REQUIRE(a.mask == b.mask);
    REQUIRE(a.ignored == "guest_ccd");
    REQUIRE(FormatControl(b, b.mask).starts_with("set=A.2 on=- off=- ignored=guest_ccd mask="));
    REQUIRE(FormatControl(a, a.mask) != FormatControl(b, b.mask));
}

TEST_CASE("Nx2Perf acquire_queued_release lives in the high word", "[nx2_perf]") {
    static_assert(Index(Opt::AcquireQueuedRelease) == 66);
    REQUIRE(opt_names[Index(Opt::AcquireQueuedRelease)] == "acquire_queued_release");
    REQUIRE(Bit(Opt::AcquireQueuedRelease) == OffMask{0, 4});
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::AcquireQueuedRelease)));
    const auto on = ParseOffMask(false, "acquire_queued_release", "", false);
    REQUIRE(on == (always_off & ~Bit(Opt::AcquireQueuedRelease)));
    REQUIRE(!IsOff(on, Index(Opt::AcquireQueuedRelease)));
    REQUIRE(IsOff(on, Index(Opt::DmaForwardMemo)));
    REQUIRE(FormatControl(ParseControl(on, "", live_safe), on)
                .ends_with(fmt::format("mask=0x{:016x}{:016x}", on.hi, on.lo)));
}

TEST_CASE("Nx2Perf live-safe switches are all named", "[nx2_perf]") {
    for (size_t i = opt_names.size(); i < 128; ++i) {
        REQUIRE(!IsOff(live_safe, i));
    }
    REQUIRE(!IsOff(live_safe, Index(Opt::PreflushAging)));
    // No On() call site: a live flip would change nothing, so it must land in ignored=.
    const auto startup = ParseOffMask(false, "", "", false);
    const ControlResult dead = ParseControl(
        startup, "off bank_check,write_intersections,immediate_callbacks\n", live_safe);
    REQUIRE(dead.mask == startup);
    REQUIRE(dead.ignored == "bank_check,write_intersections,immediate_callbacks");
}

TEST_CASE("Nx2Perf inflight_wait_v2 is off by default and keeps the inflight waits off",
          "[nx2_perf]") {
    static_assert(Index(Opt::InflightWaitV2) == 68);
    REQUIRE(opt_names[Index(Opt::InflightWaitV2)] == "inflight_wait_v2");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::InflightWaitV2)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::InflightWaitV2)));
    REQUIRE(IsOff(ParseOffMask(true, "", "", false), Index(Opt::InflightWaitV2)));
    REQUIRE(ParseOffMask(false, "inflight_wait_v2", "", false) ==
            (always_off & ~Bit(Opt::InflightWaitV2)));
    if (std::getenv("EDEN_NX2_PERF_ON") == nullptr) {
        // This process's own mask: nothing named the switch or the old pair.
        REQUIRE(!On(Opt::InflightWaitV2));
        REQUIRE(!VideoCommon::Nx2Inflight::WriteWait());
        REQUIRE(!VideoCommon::Nx2Inflight::ReadExact());
        REQUIRE(!VideoCommon::Nx2Inflight::Any());
    }
}

namespace {
namespace TapeTest {

namespace DmaForward = VideoCommon::DmaForward;

constexpr u32 MacroRegistersStart = 0xe00;

/// One Turing macro instruction; idle lanes OR zero into the discard register (no carry).
struct Ins {
    std::array<u32, 2> op{14, 14};
    std::array<u32, 2> dst{24, 24};
    std::array<u32, 2> a{24, 24};
    std::array<u32, 2> b{24, 24};
    std::array<u32, 2> imm{0, 0};
    std::array<u32, 2> method{0, 0};
    std::array<u32, 2> emit{0, 0};
    u32 pred_reg = 24;
    u32 pred_mode = 0;
    bool end = false;
};

void Append(std::vector<u32>& code, const Ins& in) {
    std::array<u32, 3> bits{};
    const auto set = [&](u32 low, u32 count, u32 value) {
        for (u32 i = 0; i < count; ++i) {
            if ((value >> i) & 1) {
                bits[(low + i) / 32] |= 1U << ((low + i) % 32);
            }
        }
    };
    set(0, 1, in.end ? 1 : 0);
    set(1, 4, in.pred_mode);
    set(5, 5, in.pred_reg);
    for (u32 lane = 0; lane < 2; ++lane) {
        set(10 + lane * 36, 5, in.op[lane]);
        set(15 + lane * 36, 5, in.dst[lane]);
        set(20 + lane * 36, 5, in.a[lane]);
        set(25 + lane * 36, 5, in.b[lane]);
        set(30 + lane * 36, 16, in.imm[lane]);
        set(82 + lane * 7, 3, in.method[lane]);
        set(85 + lane * 7, 4, in.emit[lane]);
    }
    code.push_back(bits[2]);
    code.push_back(bits[1]);
    code.push_back(bits[0]);
}

/// MME DMA read of `words` words from the parameters' address, then `body`, then a delay slot.
std::vector<u32> Macro(u32 words, std::vector<Ins> body) {
    std::vector<u32> code;
    Ins address;
    address.method[0] = 5; // 0x154, increment 1
    address.imm[0] = 0x1154;
    address.emit = {3, 4};
    Append(code, address);
    Ins read;
    read.method[0] = 5;
    read.imm[0] = 0x158;
    read.emit[0] = 6;
    read.imm[1] = words;
    Append(code, read);
    body.back().end = true;
    for (const Ins& in : body) {
        Append(code, in);
    }
    Append(code, Ins{});
    return code;
}

Ins Load(u32 reg0, u32 reg1) {
    Ins in;
    in.op = {0, 0};
    in.dst = {reg0, reg1};
    in.a = {28, 29};
    return in;
}

Ins Lanes(u32 op0, u32 dst0, u32 a0, u32 b0, u32 op1, u32 dst1, u32 a1, u32 b1) {
    Ins in;
    in.op = {op0, op1};
    in.dst = {dst0, dst1};
    in.a = {a0, a1};
    in.b = {b0, b1};
    return in;
}

/// Emits two registers to consecutive methods from `first` (0 keeps the method going).
Ins Emit(u32 reg0, u32 reg1, u32 first) {
    Ins in;
    in.a = {reg0, reg1};
    in.emit = {1, 2};
    if (first != 0) {
        in.method[0] = 5;
        in.imm[0] = 0x1000 | first;
    }
    return in;
}

DmaForward::Plan Analyze(const std::vector<u32>& code) {
    const std::array<u32, 2> parameters{0x21, 0x13960e00};
    static std::array<u32, 0xc00> ram{};
    return DmaForward::Analyze<true>(
        code, 0, parameters, ram, MacroRegistersStart,
        [](u32, bool& unknown) {
            unknown = false;
            return 0U;
        },
        [](u32 method) { return method >= 0x500 && method < 0x510; });
}

/// EvalTape against Recompute over random words (both senses of equality and carries).
void CheckTape(const DmaForward::Plan& plan, u32 seed) {
    REQUIRE(plan.eligible);
    REQUIRE(plan.tape_valid);
    REQUIRE(plan.tape.num_ops <= DmaForward::MaxTapeOps);
    std::mt19937 rng{seed};
    constexpr std::array<u32, 6> special{0, 1, 2, ~0U, 0x80000000U, 0x7fffffffU};
    for (u32 iteration = 0; iteration < 10000; ++iteration) {
        std::array<u32, DmaForward::TapeSlots> slots{};
        for (u32 i = 0; i < plan.snapshot.dma_words; ++i) {
            slots[i] = (rng() & 1) ? static_cast<u32>(rng()) : special[rng() % special.size()];
        }
        std::vector<std::pair<u32, u32>> expected;
        REQUIRE(DmaForward::Recompute(
            plan.snapshot, std::span<const u32>{slots.data(), plan.snapshot.dma_words},
            MacroRegistersStart,
            [&](u32 method, u32 value) { expected.emplace_back(method, value); }));
        std::vector<std::pair<u32, u32>> got;
        DmaForward::EvalTape(plan.tape, slots,
                             [&](u32 method, u32 value) { got.emplace_back(method, value); });
        REQUIRE(got == expected);
    }
}

} // namespace TapeTest
} // namespace

TEST_CASE("dma_forward_tape: add and subtract with carry", "[nx2_perf]") {
    using namespace TapeTest;
    const auto code = Macro(4, {Load(0, 1), Load(2, 3), Lanes(2, 4, 0, 2, 3, 5, 1, 3),
                                Lanes(0, 6, 0, 2, 1, 7, 1, 3), Lanes(5, 8, 24, 24, 14, 24, 24, 24),
                                Emit(4, 5, 0x500), Emit(7, 8, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    REQUIRE(plan.tape.num_writes == 4);
    CheckTape(plan, 1);
}

TEST_CASE("dma_forward_tape: multiplies and count leading zeros", "[nx2_perf]") {
    using namespace TapeTest;
    const auto code = Macro(2, {Load(0, 1), Lanes(4, 2, 0, 1, 5, 3, 24, 24),
                                Lanes(6, 4, 0, 1, 5, 5, 24, 24), Lanes(8, 6, 0, 24, 14, 24, 24, 24),
                                Emit(2, 3, 0x500), Emit(5, 6, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    CheckTape(plan, 2);
}

TEST_CASE("dma_forward_tape: shifts", "[nx2_perf]") {
    using namespace TapeTest;
    Ins shifts = Lanes(11, 4, 0, 1, 9, 5, 25, 1); // sar r0, r1; imm << r1
    shifts.imm[1] = 3;
    Ins constant_shift = Lanes(9, 6, 0, 25, 14, 24, 24, 24); // r0 << 5
    constant_shift.imm[0] = 5;
    const auto code = Macro(2, {Load(0, 1), Lanes(9, 2, 0, 1, 10, 3, 0, 1), shifts,
                                constant_shift, Emit(2, 3, 0x500), Emit(4, 5, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    CheckTape(plan, 3);
    const auto plan6 = Analyze(Macro(2, {Load(0, 1), constant_shift, Emit(6, 6, 0x500)}));
    INFO(plan6.reason);
    CheckTape(plan6, 4);
}

TEST_CASE("dma_forward_tape: merge and logic", "[nx2_perf]") {
    using namespace TapeTest;
    Ins constant = Lanes(14, 9, 27, 24, 14, 24, 24, 24); // r9 = 0x12345678
    constant.imm = {0x1234, 0x5678};
    Ins merges = Lanes(16, 2, 0, 1, 16, 3, 9, 0);
    merges.imm = {(8 << 10) | (12 << 5) | 4, (20 << 10) | (9 << 5) | 30};
    const auto code =
        Macro(2, {Load(0, 1), constant, merges, Lanes(12, 4, 0, 1, 13, 5, 0, 1),
                  Lanes(15, 6, 4, 5, 14, 7, 0, 1), Emit(2, 3, 0x500), Emit(6, 7, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    CheckTape(plan, 5);
}

TEST_CASE("dma_forward_tape: compares", "[nx2_perf]") {
    using namespace TapeTest;
    const auto code = Macro(2, {Load(0, 1), Lanes(17, 2, 0, 1, 18, 3, 0, 1),
                                Lanes(19, 4, 0, 1, 20, 5, 0, 1), Lanes(21, 6, 0, 1, 14, 24, 24, 24),
                                Lanes(15, 7, 5, 6, 14, 24, 24, 24), Emit(2, 3, 0x500),
                                Emit(4, 7, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    CheckTape(plan, 6);
}

TEST_CASE("dma_forward_tape: lanes on a predicate from the DMA words", "[nx2_perf]") {
    using namespace TapeTest;
    Ins init = Lanes(14, 3, 25, 24, 14, 24, 24, 24); // r3 = 100
    init.imm[0] = 100;
    // Mode 5: lane 0 runs when r0 is zero, lane 1 when it isn't; outputs always.
    Ins predicated = Lanes(0, 3, 1, 0, 15, 1, 1, 0);
    predicated.pred_reg = 0;
    predicated.pred_mode = 5;
    const auto code = Macro(2, {Load(0, 1), init, predicated,
                                Lanes(5, 8, 24, 24, 14, 24, 24, 24), Emit(3, 1, 0x500),
                                Emit(8, 8, 0)});
    const auto plan = Analyze(code);
    INFO(plan.reason);
    CheckTape(plan, 7);

    // The ALU outputs of predicated lanes are zero when the lane doesn't run.
    Ins outputs = Lanes(0, 24, 0, 25, 15, 24, 0, 27);
    outputs.pred_reg = 0;
    outputs.pred_mode = 5;
    outputs.imm = {9, 0x1500};
    outputs.method[0] = 6;
    outputs.emit = {1, 2};
    const auto plan2 = Analyze(Macro(1, {Lanes(0, 0, 28, 24, 14, 24, 24, 24), outputs}));
    INFO(plan2.reason);
    CheckTape(plan2, 8);
}

TEST_CASE("dma_forward_tape: a tape that doesn't fit falls back", "[nx2_perf]") {
    using namespace TapeTest;
    std::vector<Ins> body{Load(0, 1), Lanes(15, 0, 0, 1, 14, 24, 24, 24)};
    for (u32 i = 0; i < 8; ++i) {
        body.push_back(Lanes(15, 0, 0, 28, 14, 24, 24, 24));
    }
    body.push_back(Emit(0, 0, 0x500));
    const auto plan = Analyze(Macro(10, body));
    INFO(plan.reason);
    REQUIRE(plan.eligible);
    REQUIRE_FALSE(plan.tape_valid);
    REQUIRE(plan.tape_fallback == DmaForward::TapeFallback::Overflow);
    // One op fewer fits.
    body.erase(body.begin() + 2);
    const auto fits = Analyze(Macro(9, body));
    INFO(fits.reason);
    CheckTape(fits, 9);
    REQUIRE(fits.tape.num_ops == DmaForward::MaxTapeOps);
}

TEST_CASE("exec_same_drop: off by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ExecSameDrop)] == "exec_same_drop");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::ExecSameDrop)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::ExecSameDrop)));
    REQUIRE(ParseOffMask(false, "exec_same_drop", "", false) ==
            (always_off & ~Bit(Opt::ExecSameDrop)));
}

TEST_CASE("exec_same_drop: a bind repeats only the last one sent on its stage", "[nx2_perf]") {
    ExecBindMemo memo;
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1000, 0x100, 7, 3));
    memo.Record(0, 0x31, 1, 0x1000, 0x100, 7, 3);
    REQUIRE(memo.Same(0, 0x31, 1, 0x1000, 0x100, 7, 3));
    // Another stage, word, address, size, queued function or GPU map.
    REQUIRE_FALSE(memo.Same(1, 0x31, 1, 0x1000, 0x100, 7, 3));
    REQUIRE_FALSE(memo.Same(0, 0x30, 1, 0x1000, 0x100, 7, 3));
    REQUIRE_FALSE(memo.Same(0, 0x31, 2, 0x1000, 0x100, 7, 3));
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1100, 0x100, 7, 3));
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1000, 0x200, 7, 3));
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1000, 0x100, 8, 3));
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1000, 0x100, 7, 4));
    // A bind of another slot on the stage replaces it.
    memo.Record(0, 0x41, 1, 0x2000, 0x100, 7, 3);
    REQUIRE_FALSE(memo.Same(0, 0x31, 1, 0x1000, 0x100, 7, 3));
    REQUIRE(memo.Same(0, 0x41, 1, 0x2000, 0x100, 7, 3));
    memo.Record(4, 0x11, 0, 0x3000, 0x40, 7, 3);
    memo.Forget(0);
    REQUIRE_FALSE(memo.Same(0, 0x41, 1, 0x2000, 0x100, 7, 3));
    REQUIRE(memo.Same(4, 0x11, 0, 0x3000, 0x40, 7, 3));
    memo.Clear();
    REQUIRE_FALSE(memo.Same(4, 0x11, 0, 0x3000, 0x40, 7, 3));
}

TEST_CASE("back_replay_lean: off by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::BackReplayLean)] == "back_replay_lean");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::BackReplayLean)));
    REQUIRE(IsOff(ParseOffMask(false, "", "", false), Index(Opt::BackReplayLean)));
    REQUIRE(ParseOffMask(false, "back_replay_lean", "", false) ==
            (always_off & ~Bit(Opt::BackReplayLean)));
}

TEST_CASE("back_replay_lean: a mixed ring replays as Execute does and stops where BackLoop "
          "releases",
          "[nx2_perf]") {
    using Tegra::Engines::Maxwell3D;
    using Access = VideoCommon::GpuSplitLeanTestAccess;
    using Control = Maxwell3D::BackRunControl;
    using Reason = Control::Reason;
    constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
    constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
    constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
    constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
    constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
    constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
    constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);
    constexpr std::array<u32, 9> executable{
        MAXWELL3D_REG_INDEX(vertex_buffer.first),
        MAXWELL3D_REG_INDEX(vertex_buffer.count),
        MAXWELL3D_REG_INDEX(index_buffer.first),
        MAXWELL3D_REG_INDEX(index_buffer.count),
        MAXWELL3D_REG_INDEX(topology_override),
        MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
        MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
        Maxwell3D::RootTables::SelectMethod,
        Maxwell3D::RootTables::LoadMethod,
    };

    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    // [0] Execute's unit boundaries, [1] back_replay_lean off, [2] on.
    std::array<std::unique_ptr<Maxwell3D>, 3> engines;
    std::array<std::unique_ptr<VideoCommon::GpuSplit>, 3> splits;
    std::array<u32, 3> calls{};
    for (size_t e = 0; e < engines.size(); ++e) {
        engines[e] = std::make_unique<Maxwell3D>(memory);
        Maxwell3D& engine = *engines[e];
        engine.fast_maxwell3d = true;
        engine.BindRasterizer(&rasterizer);
        VideoCommon::Dirty::SetupDirtyFlags(engine.dirty.tables);
        // Every register gets a flag, so that a missed or extra write shows.
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (engine.dirty.tables[1][method] == 0) {
                engine.dirty.tables[1][method] =
                    static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
            }
        }
        engine.dirty.flags.reset();
        // An unmapped buffer: uploads write nothing, the offset still advances.
        engine.CallMethod(system, CbAddressHigh, 0x1, true);
        engine.CallMethod(system, CbAddressLow, 0x2000'0000, true);
        engine.CallMethod(system, CbSize, 0x10000, true);
        splits[e] = std::make_unique<VideoCommon::GpuSplit>();
        Access::Initialize(*splits[e], system);
    }
    std::vector<u32> state_methods;
    for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
        if (!engines[0]->execution_mask[method] && (method < CbSize || method > CbAddressLow) &&
            method != ShadowControl) {
            state_methods.push_back(method);
        }
    }

    // The same ring for each engine: state runs (Method and macro writes), constant buffer
    // words, draws, other executable writes, counted draw.begin repeats, Call entries,
    // segment entries (the method sink) and shadow RAM control writes.
    const GPUVAddr segment = 0x1234'0000;
    for (size_t e = 0; e < engines.size(); ++e) {
        auto& split = *splits[e];
        Maxwell3D* const engine = engines[e].get();
        u32* const counter = &calls[e];
        std::mt19937 rng{7};
        for (u32 step = 0; step < 6000; ++step) {
            const u32 kind = rng() % 100;
            if (kind < 35) {
                const u32 run = 1 + rng() % 12;
                for (u32 i = 0; i < run; ++i) {
                    const u32 method = state_methods[rng() % state_methods.size()];
                    const u32 value = rng() % 4;
                    if (rng() % 3 == 0) {
                        split.MacroWrite(engine, method, value);
                    } else {
                        split.Method(engine, method, value, rng() % 2 == 0, nullptr);
                    }
                }
            } else if (kind < 50) {
                if (rng() % 4 == 0) {
                    split.Method(engine, CbOffset, (rng() % 0x400) * 4, true, nullptr);
                }
                const u32 run = 1 + rng() % 8;
                for (u32 i = 0; i < run; ++i) {
                    split.MacroWrite(engine, CbData + rng() % 16, rng());
                }
            } else if (kind < 65) {
                split.MacroWrite(engine, DrawBegin, rng() % 8);
                split.MacroWrite(engine, executable[0], rng() % 4);
                split.MacroWrite(engine, executable[1], 3 + rng() % 4);
                split.MacroWrite(engine, DrawEnd, 0);
            } else if (kind < 78) {
                const u32 method = executable[rng() % executable.size()];
                split.Method(engine, method, rng() % 64, rng() % 2 == 0, nullptr);
            } else if (kind < 84) {
                const u32 begin = (1u << 26) | (rng() % 8);
                const u32 repeats = 1 + rng() % 3;
                for (u32 i = 0; i < repeats; ++i) {
                    split.RepeatableMethod(engine, DrawBegin, begin, true, true,
                                           VideoCommon::GpuSplit::Effect::Unknown, false);
                }
                split.MacroWrite(engine, DrawEnd, 0);
            } else if (kind < 90) {
                split.Defer([counter] { ++*counter; });
            } else if (kind < 95) {
                split.Method(engine, state_methods[rng() % state_methods.size()], rng() % 4,
                             true, &segment);
            } else {
                split.Method(engine, ShadowControl, rng() % 4, true, nullptr);
            }
        }
    }
    const u64 end = Access::End(*splits[0]);
    REQUIRE(Access::End(*splits[1]) == end);
    REQUIRE(Access::End(*splits[2]) == end);

    std::vector<u64> boundaries;
    for (u64 position = 0; position < end;) {
        position = Access::Step(*splits[0], false, position, end, 0);
        boundaries.push_back(position);
    }

    const auto compare = [&] {
        const Maxwell3D& a = *engines[1];
        const Maxwell3D& b = *engines[2];
        REQUIRE(a.regs.reg_array == b.regs.reg_array);
        REQUIRE(a.shadow_state.reg_array == b.shadow_state.reg_array);
        REQUIRE(a.shadow_state.shadow_ram_control == b.shadow_state.shadow_ram_control);
        REQUIRE(a.dirty.flags == b.dirty.flags);
        REQUIRE(a.method_sink == b.method_sink);
        REQUIRE(a.draw_manager.draw_state.instance_count ==
                b.draw_manager.draw_state.instance_count);
        REQUIRE(a.draw_manager.draw_state.drawn_instances ==
                b.draw_manager.draw_state.drawn_instances);
        REQUIRE(a.draw_manager.HasDeferredDraw() == b.draw_manager.HasDeferredDraw());
        REQUIRE(calls[1] == calls[2]);
    };

    std::mt19937 rng{11};
    u64 off_position = 0;
    u64 position = 0;
    u64 last_store = 0;
    size_t next = 0; // The first boundary after `position`
    while (position < end) {
        CAPTURE(position);
        // A release target ahead (at or inside a unit), a requested hook, or neither.
        u64 target = ~u64{0};
        bool hook = false;
        const u32 pick = rng() % 10;
        if (pick < 3) {
            target = boundaries[std::min<size_t>(next + rng() % 24, boundaries.size() - 1)];
            if (rng() % 2 == 0 && target > position + 1) {
                target -= 1;
            }
        } else if (pick == 3) {
            hook = true;
        }
        Access::SetRelease(*splits[2], target, hook);
        // Where BackLoop gets the position back: after one Execute unless BackRun takes the
        // entry, then at the first boundary that ends the run.
        u64 expected = boundaries[next];
        if (Control::Classify(Access::Word(*splits[2], position)) == Reason::None) {
            for (size_t i = next; i < boundaries.size(); ++i) {
                const u64 b = boundaries[i];
                expected = b;
                if (b == end || hook || b >= target || b - last_store >= Control::StoreEvery ||
                    Control::Classify(Access::Word(*splits[2], b)) != Reason::None) {
                    break;
                }
            }
        }
        const u64 lean_position = Access::Step(*splits[2], true, position, end, last_store);
        REQUIRE(lean_position == expected);
        while (off_position < lean_position) {
            off_position = Access::Step(*splits[1], false, off_position, end, 0);
        }
        REQUIRE(off_position == lean_position);
        compare();
        position = lean_position;
        while (next < boundaries.size() && boundaries[next] <= position) {
            ++next;
        }
        // BackLoop stores `consumed` at a release and every 4096 words.
        if (position >= target || position - last_store >= Control::StoreEvery) {
            last_store = position;
        }
    }
    REQUIRE(Access::PastTarget(*splits[2]) == 0);
    // Some runs took more than one unit.
    REQUIRE(Access::LeanUnits(*splits[2]) > Access::LeanCalls(*splits[2]));
    REQUIRE(calls[1] == calls[0]);
    REQUIRE(calls[1] > 0);
}
