// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_state.h"

using Service::Nx2::NativePresentRequest;
using Service::Nx2::NativePresentState;

namespace {
constexpr u64 Layer = 0x102345678ULL;
NativePresentRequest Request(u8 credit, u32 image = 7, u64 layer = Layer) {
    return {.layer = layer, .image = image, .credit = credit};
}
}

TEST_CASE("Native present request ignores only defined slot padding", "[core][nx2_present_state]") {
    std::array<u32, 26> packet{};
    packet[0] = 245;
    packet[1] = 24;
    packet[2] = static_cast<u32>(Layer);
    packet[3] = static_cast<u32>(Layer >> 32);
    packet[4] = 0x4f434601;
    packet[5] = 42;
    packet[22] = ~u32{6};
    packet[24] = 128;
    const auto request = NativePresentRequest::Parse(packet);
    REQUIRE(request);
    CHECK(request->layer == Layer);
    CHECK(request->credit == 1);
    CHECK(request->image == 42);
    CHECK(request->render_sync[2] == 128);
    CHECK_FALSE(NativePresentRequest::Parse(std::span{packet}.first(25)));
    packet[23] = 1;
    CHECK_FALSE(NativePresentRequest::Parse(packet));
    packet[23] = 0;
    packet[25] = 1;
    CHECK_FALSE(NativePresentRequest::Parse(packet));
    packet[25] = 0;
    packet[4] = 0x4f43460f;
    CHECK_FALSE(NativePresentRequest::Parse(packet));
    packet[4] = 0;
    packet[22] = ~u32{192};
    CHECK_FALSE(NativePresentRequest::Parse(packet));
    packet[22] = packet[24] = 0;
    CHECK(NativePresentRequest::Parse(packet));
}

TEST_CASE("Real presentation credits return only after completion", "[core][nx2_present_state]") {
    NativePresentState state;
    REQUIRE(state.Register(Layer, 2, 3, 9, 40));
    REQUIRE(state.Available(Layer) == u64{3});
    CHECK(state.ReleasableCounters() == std::vector<u32>{9});
    const auto a = state.Submit(Request(0, 5));
    const auto b = state.Submit(Request(1, 6));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(state.Available(Layer) == u64{0});
    CHECK(state.ReleasableCounters().empty());
    CHECK_FALSE(state.Submit(Request(0)));
    CHECK_FALSE(state.CancelCredit(Layer, 0));
    CHECK_FALSE(state.HasNotifications());
    const auto reply = b->Reply(0);
    CHECK(reply == std::array<u32, 6>{0, 0, ~u32{9}, 0, 42, 0});
    const auto second = state.Complete(*b, true);
    REQUIRE(second.accepted);
    CHECK(second.increments == 0); // Later image cannot prematurely release the earlier ticket.
    CHECK(state.ReleasableCounters().empty()); // Old first-ticket waiter still owns its target.
    CHECK(state.Available(Layer) == u64{2});
    const auto notifications = state.TakeNotifications();
    CHECK(notifications[0] == (1U << 3)); // TIPC201's SECOND index, not the first or native-vsync.
    CHECK_FALSE(state.HasNotifications());
    const auto first = state.Complete(*a, true);
    CHECK(first.increments == 2);
    CHECK(first.counter == 9);
    CHECK(state.Available(Layer) == u64{3});
    CHECK(state.ReleasableCounters() == std::vector<u32>{9});
    CHECK(state.HasNotifications()); // Completion after a drain produces a new notification.
    state.RestoreNotifications(notifications); // Failed buffer delivery cannot lose an older wake.
    CHECK(state.TakeNotifications()[0] == (1U << 3));
    CHECK_FALSE(state.Complete(*a, true).accepted);
    CHECK_FALSE(state.Complete(*b, true).accepted);
}

TEST_CASE("Failed composition faults its layer without satisfying a handed fence",
          "[core][nx2_present_state]") {
    NativePresentState state;
    REQUIRE(state.Register(Layer, 2, 3, 9, 0));
    const auto a = state.Submit(Request(0));
    REQUIRE(a);
    const auto failed = state.Complete(*a, false);
    CHECK(failed.accepted);
    CHECK(failed.failed);
    CHECK(failed.increments == 0);
    CHECK(state.ReleasableCounters().empty()); // Do not satisfy a failed waiter through ID reuse.
    CHECK_FALSE(state.Available(Layer));
    CHECK_FALSE(state.Submit(Request(1)));
    CHECK(state.TakeNotifications()[0] == (1U << 3));
    CHECK_FALSE(state.Complete(*a, true).accepted);
    CHECK_FALSE(state.Complete(*a, false).accepted);
}

TEST_CASE("Retired layers keep fences and reject late callbacks after token reuse",
          "[core][nx2_present_state]") {
    NativePresentState state;
    REQUIRE(state.Register(Layer, 2, 3, 9, 17));
    const auto old = state.Submit(Request(0));
    REQUIRE(old);
    REQUIRE(state.Unregister(Layer));
    CHECK(state.CollectRetiredCounters().empty());
    REQUIRE(state.Register(Layer, 2, 3, 10, 0));
    const auto completion = state.Complete(*old, true);
    CHECK(completion.counter == 9);
    CHECK(completion.increments == 1);
    CHECK_FALSE(state.HasNotifications()); // Old generation cannot wake the new layer.
    CHECK(state.Available(Layer) == u64{3});
    CHECK(state.CollectRetiredCounters() == std::vector<u32>{9});
    CHECK_FALSE(state.Complete(*old, true).accepted);
}

TEST_CASE("Rejected present rolls back only an unpublished latest ticket",
          "[core][nx2_present_state]") {
    NativePresentState state;
    REQUIRE(state.Register(Layer, 2, 3, 9, 0));
    const auto a = state.Submit(Request(0));
    const auto b = state.Submit(Request(1));
    REQUIRE(a);
    REQUIRE(b);
    CHECK_FALSE(state.AbortUnpublished(*a));
    REQUIRE(state.AbortUnpublished(*b));
    CHECK(state.Available(Layer) == u64{2});
    const auto replacement = state.Submit(Request(1, 20));
    REQUIRE(replacement);
    CHECK(replacement->value == b->value);
    CHECK_FALSE(state.Complete(*b, true).accepted); // Different image, no old callback was handed out.
    CHECK(state.Complete(*a, true).increments == 1);
    CHECK(state.Complete(*replacement, true).increments == 1);
}

TEST_CASE("Notification and counter bounds are explicit", "[core][nx2_present_state]") {
    NativePresentState state;
    CHECK_FALSE(state.Register(Layer, 2, 1024, 9, 0));
    CHECK_FALSE(state.Register(Layer, 2, 2, 9, 0));
    CHECK_FALSE(state.Register(Layer, 2, 3, 192, 0));
    REQUIRE(state.Register(Layer, 1022, 1023, 9, std::numeric_limits<u32>::max() - 1));
    CHECK_FALSE(state.Register(Layer + 1, 4, 1022, 10, 0));
    CHECK_FALSE(state.Register(Layer + 1, 4, 5, 9, 0));
    const auto last = state.Submit(Request(0));
    REQUIRE(last);
    CHECK(last->value == std::numeric_limits<u32>::max());
    CHECK_FALSE(state.Submit(Request(1)));
    CHECK(state.Complete(*last, true).increments == 1);
    const auto bits = state.TakeNotifications();
    CHECK(bits.back() == 0x80000000U);
    for (size_t i = 0; i + 1 < bits.size(); ++i) CHECK(bits[i] == 0);
}
