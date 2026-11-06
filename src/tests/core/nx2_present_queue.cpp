// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nx2_present_queue.h"
#include "video_core/host1x/host1x.h"

namespace {
using namespace std::chrono_literals;
constexpr u32 ReleaseId = 1;
constexpr u32 FrameId = 2;

struct Fixture {
    Fixture() {
        timing.SetMulticore(false);
        timing.Initialize([] {});
    }

    void Advance() {
        timing.AddTicks(1'000'000'000);
        timing.Advance();
    }

    Core::Timing::CoreTiming timing;
    Tegra::Host1x::SyncpointManager syncpoints;
};
} // namespace

/// Checks both completion orders so neither the deadline nor GPU completion releases alone.
TEST_CASE("Native present release needs both the deadline and GPU fence", "[nx2_present]") {
    Fixture f;
    Service::Nx2PresentReleaseQueue queue{f.timing, f.syncpoints, ReleaseId};
    REQUIRE(queue.Queue(10ms, FrameId, 1) == 1U);

    SECTION("GPU finishes first") {
        f.syncpoints.IncrementGuest(FrameId);
        f.timing.Advance();
        REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 0);
        f.Advance();
    }
    SECTION("Presentation deadline passes first") {
        f.Advance();
        REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 0);
        f.Advance();
        REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 0);
        f.syncpoints.IncrementGuest(FrameId);
        f.Advance();
    }
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 1);
    REQUIRE_FALSE(f.timing.Advance().has_value());
}

/// Checks FIFO release gating and timer rearming after the queue becomes empty.
TEST_CASE("Native present releases remain ordered when a later frame is ready", "[nx2_present]") {
    Fixture f;
    Service::Nx2PresentReleaseQueue queue{f.timing, f.syncpoints, ReleaseId};
    REQUIRE(queue.Queue(0ns, FrameId, 1) == 1U);
    REQUIRE(queue.Queue(0ns, ~0U, 0) == 2U);
    f.timing.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 0);
    f.syncpoints.IncrementGuest(FrameId);
    f.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 2);

    // An empty queue must schedule its timer again on the next submission.
    REQUIRE(queue.Queue(0ns, ~0U, 0) == 3U);
    f.timing.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 3);
    REQUIRE_FALSE(f.timing.Advance().has_value());
}

/// Verifies saturation rejects submissions until completed frames restore queue capacity.
TEST_CASE("Native present queue bounds pending releases and recovers capacity", "[nx2_present]") {
    Fixture f;
    Service::Nx2PresentReleaseQueue queue{f.timing, f.syncpoints, ReleaseId};
    for (u32 value = 1; value <= Service::Nx2PresentReleaseQueue::Capacity; ++value) {
        REQUIRE(queue.Queue(0ns, FrameId, 1) == value);
    }
    REQUIRE_FALSE(queue.Queue(0ns, FrameId, 1).has_value());
    f.timing.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 0);
    f.syncpoints.IncrementGuest(FrameId);
    f.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 15);
    REQUIRE(queue.Queue(0ns, ~0U, 0) == 16U);
    f.timing.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == 16);
}

/// Verifies shutdown cancels callbacks without advancing counters already at or past the target.
TEST_CASE("Native present shutdown cancels retries and resolves only pending releases",
          "[nx2_present]") {
    Fixture f;
    u32 expected = 2;
    {
        Service::Nx2PresentReleaseQueue queue{f.timing, f.syncpoints, ReleaseId};
        REQUIRE(queue.Queue(0ns, FrameId, 1) == 1U);
        REQUIRE(queue.Queue(0ns, FrameId, 2) == 2U);
        f.timing.Advance();
        SECTION("Outstanding fences are signalled") {}
        SECTION("Counter already reached the target") {
            f.syncpoints.IncrementGuest(ReleaseId);
            f.syncpoints.IncrementGuest(ReleaseId);
        }
        SECTION("Counter already passed the target") {
            expected = 3;
            for (int i = 0; i < 3; ++i) {
                f.syncpoints.IncrementGuest(ReleaseId);
            }
        }
    }
    const auto value = f.syncpoints.GetGuestSyncpointValue(ReleaseId);
    REQUIRE(value == expected);
    REQUIRE_FALSE(f.timing.Advance().has_value());
    f.Advance();
    REQUIRE(f.syncpoints.GetGuestSyncpointValue(ReleaseId) == value);
}

/// Exhausts the allocator to check ID zero stays unreserved and freed IDs remain reusable.
TEST_CASE("Syncpoint exhaustion never reserves ID zero and freed IDs can be reused",
          "[nx2_present]") {
    Core::System system;
    SCOPE_EXIT {
        system.CoreTiming().Reset();
    };
    system.Initialize();
    auto host = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::NvCore::SyncpointManager allocator{*host};
    std::vector<u32> allocated;
    while (const auto id = allocator.TryAllocateSyncpoint(true)) {
        REQUIRE(*id != 0);
        allocated.push_back(*id);
    }
    REQUIRE_FALSE(allocated.empty());
    REQUIRE_FALSE(allocator.TryAllocateSyncpoint(true).has_value());
    REQUIRE_FALSE(allocator.IsSyncpointAllocated(0));
    allocator.FreeSyncpoint(allocated.back());
    REQUIRE(allocator.TryAllocateSyncpoint(true) == allocated.back());
    REQUIRE_FALSE(allocator.TryAllocateSyncpoint(true).has_value());
    REQUIRE_FALSE(allocator.IsSyncpointAllocated(0));
}
