// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_wait.h"

using namespace Service::Nx2;

namespace {
constexpr u64 Oot = 0x0400486024DEC000ULL;
constexpr u64 Layer = 0x123456789ULL;
constexpr PresentFence Previous{2, 100};
constexpr PresentFence Current{2, 110};
const std::optional<PresentReplyFence> Reply{PresentReplyFence{Layer, Previous}};
} // namespace

TEST_CASE("Previous-only present policy is explicit and preserves ordered release scope",
          "[core][nx2_present_wait]") {
    CHECK(PreviousPresentOnlyEnabled("1"));
    for (const auto value : {"", "0", "true", "01", "1 ", " 1", "11"}) {
        CHECK_FALSE(PreviousPresentOnlyEnabled(value));
    }
    const auto eligible = [](bool enabled, u64 program, int ahead, u64 layer,
                             PresentFence previous, PresentFence current,
                             std::optional<PresentReplyFence> reply) {
        return CanOmitCurrentPresentWait(enabled, program, ahead, layer, previous, current, reply);
    };
    CHECK(eligible(true, Oot, 1, Layer, Previous, Current, Reply));
    CHECK_FALSE(eligible(false, Oot, 1, Layer, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot + 1, 1, Layer, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, -1, Layer, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, 0, Layer, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, 0, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer + 1, Previous, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, Previous, Current, std::nullopt));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, {2, 99}, Current, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, Previous, {3, 110}, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, Previous, Previous, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, Previous, {2, 99}, Reply));
    CHECK_FALSE(eligible(true, Oot, 1, Layer, Previous, {2, 100 + 0x80000000U}, Reply));
    const PresentFence wrapped{2, 0xfffffff8U};
    CHECK_FALSE(eligible(true, Oot, 1, Layer, wrapped, {2, 8},
                         PresentReplyFence{Layer, wrapped}));
    const PresentFence invalid{192, 100};
    CHECK_FALSE(eligible(true, Oot, 1, Layer, invalid, {192, 110},
                         PresentReplyFence{Layer, invalid}));
}

TEST_CASE("Present wait executes the previous fence before the optional current fence",
          "[core][nx2_present_wait]") {
    for (const bool previous_ready : {false, true}) {
        for (const bool current_ready : {false, true}) {
            std::vector<PresentFence> waited;
            const bool result = WaitForPresentFences(Previous, Current, true, [&](PresentFence f) {
                waited.push_back(f);
                return f == Previous ? previous_ready : current_ready;
            });
            CHECK(waited == std::vector<PresentFence>{Previous, Current});
            CHECK(result == (previous_ready && current_ready));
        }
    }
}

TEST_CASE("Previous-only present cannot turn a cancelled or timed-out wait into success",
          "[core][nx2_present_wait]") {
    for (const bool previous_ready : {false, true}) {
        std::vector<PresentFence> waited;
        const bool result = WaitForPresentFences(Previous, Current, false, [&](PresentFence f) {
            waited.push_back(f);
            return previous_ready;
        });
        CHECK(waited == std::vector<PresentFence>{Previous});
        CHECK(result == previous_ready);
    }
}

TEST_CASE("Previous-only replies leave at most the current ordered Guest fence unfinished",
          "[core][nx2_present_wait]") {
    u32 guest_completed = 10;
    std::optional<PresentReplyFence> last_reply{PresentReplyFence{Layer, {2, 10}}};
    for (u32 frame = 11; frame <= 30; ++frame) {
        const PresentFence previous{2, frame - 1};
        const PresentFence current{2, frame};
        REQUIRE(CanOmitCurrentPresentWait(true, Oot, 1, Layer, previous, current, last_reply));
        size_t waits{};
        const bool ready = WaitForPresentFences(previous, current, false, [&](PresentFence fence) {
            ++waits;
            guest_completed = fence.value;
            return true;
        });
        REQUIRE(ready);
        CHECK(waits == 1);
        CHECK(guest_completed == frame - 1);
        CHECK(current.value - guest_completed == 1);
        last_reply = PresentReplyFence{Layer, current};
    }
    // This contract says nothing about physical-GPU completion or command-memory ownership.
}

TEST_CASE("Present interval mean handles startup and following 60 Hz windows exactly",
          "[core][nx2_present_wait][present_interval_stats]") {
    PresentIntervalAccumulator stats;
    CHECK(stats.MeanMicroseconds() == 0);
    // Integer-microsecond 60 Hz sequence: three intervals total exactly 50000 us.
    const auto interval = [](u32 index) { return index % 3 == 0 ? 16666LL : 16667LL; };
    for (u32 present = 0; present < 600; ++present) {
        if (present != 0) stats.Add(interval(present));
    }
    CHECK(stats.intervals == 599);
    CHECK(stats.sum_us == 9983334);
    CHECK(stats.MeanMicroseconds() == 16666);
    stats = {}; // Production reset preserves last_present, not the accumulator.
    for (u32 present = 600; present < 1200; ++present) stats.Add(interval(present));
    CHECK(stats.intervals == 600);
    CHECK(stats.sum_us == 10000000);
    CHECK(stats.MeanMicroseconds() == 16666);
    CHECK(stats.sum_us / 599 == 16694); // The former fixed divisor overstated this window.
}
