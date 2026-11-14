// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/memory_manager.h"

TEST_CASE("Entries parsed without a pending acquire keep their submit-time snapshot",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    const u64 snapshot_suspensions = dma_pusher.suspensions.load();
    for (int entry = 0; entry < 8; ++entry) {
        REQUIRE_FALSE(dma_pusher.puller.HasPendingAcquire());
        REQUIRE(dma_pusher.puller.ResumeAcquire(dma_pusher));
    }
    REQUIRE(dma_pusher.suspensions.load() == snapshot_suspensions);
}

TEST_CASE("Command list word blocks are reused by size and trimmed",
          "[video_core][dma_pusher]") {
    namespace Pool = Tegra::CommandWordsPool;
    Pool::Trim();
    REQUIRE(Pool::PooledBytes() == 0);

    void* const block = Pool::Allocate(5000);
    Pool::Deallocate(block, 5000);
    REQUIRE(Pool::PooledBytes() == 8192);
    void* const reused = Pool::Allocate(6000);
    REQUIRE(reused == block);
    REQUIRE(Pool::PooledBytes() == 0);
    Pool::Deallocate(reused, 6000);

    // 4 MiB blocks: two are kept (8 MiB a class); larger ones never are.
    constexpr std::size_t large = std::size_t{4} << 20;
    std::array<void*, 3> blocks{};
    for (void*& b : blocks) {
        b = Pool::Allocate(large);
    }
    void* const huge = Pool::Allocate(large + 1);
    for (void* const b : blocks) {
        Pool::Deallocate(b, large);
    }
    Pool::Deallocate(huge, large + 1);
    REQUIRE(Pool::PooledBytes() == 8192 + 2 * large);

    Pool::Trim();
    REQUIRE(Pool::PooledBytes() == 0);
}

TEST_CASE("A synchronous snapshot refresh waits for the list's queued copy",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    Tegra::CommandList list;
    list.submitted_offsets.push_back(0);
    list.deferred_entries.push_back(0);
    REQUIRE(dma_pusher.QueueSnapshotCopy(list, 2));
    REQUIRE(list.copy_job != 0);
    REQUIRE(list.snapshot_suspensions == 2);
    REQUIRE(list.deferred_entries.empty());

    dma_pusher.RefreshSnapshot(list, 4);
    REQUIRE(list.copy_job == 0);
    REQUIRE(list.snapshot_suspensions == 4);
    Tegra::DmaPusher::WaitForSnapshotCopies();
}
