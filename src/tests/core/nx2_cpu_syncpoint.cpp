// SPDX-License-Identifier: GPL-3.0-or-later
#include <functional>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace {
using Values = Tegra::Host1x::SyncpointManager;
using Allocator = Service::Nvidia::NvCore::SyncpointManager;
}

TEST_CASE("NX2 CPU completion waits for real callback while GPU Guest can proceed", "[nx2_cpu_syncpoint]") {
    Values values;
    REQUIRE(values.MarkCpuGpuCompletion(2));
    CHECK_FALSE(values.MarkCpuGpuCompletion(2));
    int cpu_wakes = 0;
    int gpu_wakes = 0;
    values.RegisterCpuAction(2, 1, [&] { ++cpu_wakes; });
    values.RegisterGuestAction(2, 1, [&] { ++gpu_wakes; });
    std::function<void()> completion;
    values.QueueGpuSyncpoint(2, [&](auto callback, bool strict) {
        REQUIRE(strict);
        completion = std::move(callback);
    });
    CHECK(values.GetGuestSyncpointValue(2) == 1);
    CHECK(values.GetCpuSyncpointValue(2) == 0);
    CHECK(gpu_wakes == 1);
    CHECK(cpu_wakes == 0);
    REQUIRE(completion);
    completion();
    CHECK(values.GetHostSyncpointValue(2) == 1);
    CHECK(values.GetCpuSyncpointValue(2) == 1);
    CHECK(cpu_wakes == 1);
    int inline_wakes = 0;
    values.RegisterCpuAction(2, 1, [&] { ++inline_wakes; });
    CHECK(inline_wakes == 1);
}

TEST_CASE("NX2 CPU completion leaves decoder and private Guest counters unchanged", "[nx2_cpu_syncpoint]") {
    Values values;
    for (u32 id : {8U, 9U}) {
        int wakes = 0;
        values.RegisterCpuAction(id, 1, [&] { ++wakes; });
        values.IncrementGuest(id);
        CHECK(wakes == 1);
        CHECK(values.GetCpuSyncpointValue(id) == 1);
        CHECK(values.GetHostSyncpointValue(id) == 0);
        CHECK_FALSE(values.UsesCpuGpuCompletion(id));
    }
    bool strict = true;
    values.QueueGpuSyncpoint(10, [&](auto callback, bool real) {
        strict = real;
        callback(); // Existing Immediate policy remains possible for unmarked IDs.
    });
    CHECK_FALSE(strict);
    CHECK(values.GetHostSyncpointValue(10) == 1);
}

TEST_CASE("NX2 CPU action cancellation retains the registration domain", "[nx2_cpu_syncpoint]") {
    Values values;
    REQUIRE(values.MarkCpuGpuCompletion(2));
    int cpu_wakes = 0;
    auto host = values.RegisterCpuAction(2, 1, [&] { ++cpu_wakes; });
    REQUIRE(host.host);
    values.DeregisterCpuAction(2, host);
    values.IncrementGuest(2);
    values.IncrementHost(2);
    CHECK(cpu_wakes == 0);
    auto guest = values.RegisterCpuAction(3, 1, [&] { ++cpu_wakes; });
    REQUIRE_FALSE(guest.host);
    values.DeregisterCpuAction(3, guest);
    values.IncrementGuest(3);
    CHECK(cpu_wakes == 0);
}

TEST_CASE("NX2 strict allocator quarantines GPU IDs and never converts reused decoder IDs", "[nx2_cpu_syncpoint]") {
    Values values;
    Allocator allocator{values};
    const auto decoder = allocator.AllocateSyncpoint(false);
    values.IncrementGuest(decoder);
    allocator.FreeSyncpoint(decoder);
    const auto gpu = allocator.TryAllocateGpuCompletionSyncpoint();
    REQUIRE(gpu);
    CHECK(*gpu != decoder);
    CHECK(values.UsesCpuGpuCompletion(*gpu));
    std::function<void()> completion;
    values.QueueGpuSyncpoint(*gpu, [&](auto callback, bool strict) {
        REQUIRE(strict);
        completion = std::move(callback);
    });
    allocator.FreeSyncpoint(*gpu);
    CHECK(allocator.IsSyncpointAllocated(*gpu));
    const auto private_id = allocator.AllocateSyncpoint(true);
    CHECK(private_id != *gpu);
    CHECK_FALSE(values.UsesCpuGpuCompletion(private_id));
    completion();
    CHECK(values.GetHostSyncpointValue(*gpu) == 1);
    CHECK(values.GetHostSyncpointValue(private_id) == 0);
    std::vector<u32> remaining;
    while (auto id = allocator.TryAllocateGpuCompletionSyncpoint()) {
        remaining.push_back(*id);
        allocator.FreeSyncpoint(*id);
    }
    REQUIRE_FALSE(remaining.empty());
    CHECK_FALSE(allocator.TryAllocateGpuCompletionSyncpoint());
    CHECK_FALSE(values.MarkCpuGpuCompletion(0));
    CHECK_FALSE(values.MarkCpuGpuCompletion(192));
    CHECK_FALSE(values.MarkCpuGpuCompletion(decoder));
}

TEST_CASE("NX2 completion quarantine and allocation history survive NvCore recreation", "[nx2_cpu_syncpoint]") {
    Values values;
    u32 gpu_id{};
    u32 previous_guest{};
    {
        Allocator first{values};
        previous_guest = first.AllocateSyncpoint(false); // Zero-valued old IDs are not fresh.
        first.FreeSyncpoint(previous_guest);
        auto gpu = first.TryAllocateGpuCompletionSyncpoint();
        REQUIRE(gpu);
        gpu_id = *gpu;
        first.FreeSyncpoint(gpu_id);
    }
    Allocator next{values};
    CHECK(next.IsSyncpointAllocated(gpu_id));
    const auto ordinary = next.AllocateSyncpoint(true);
    CHECK(ordinary != gpu_id);
    CHECK_FALSE(values.UsesCpuGpuCompletion(ordinary));
    auto fresh = next.TryAllocateGpuCompletionSyncpoint();
    REQUIRE(fresh);
    CHECK(*fresh != gpu_id);
    CHECK(*fresh != previous_guest);
    CHECK(*fresh != ordinary);
}
