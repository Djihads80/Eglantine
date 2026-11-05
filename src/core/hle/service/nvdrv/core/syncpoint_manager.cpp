// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/assert.h"
#include "common/logging.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "video_core/host1x/host1x.h"

namespace Service::Nvidia::NvCore {

SyncpointManager::SyncpointManager(Tegra::Host1x::Host1x& host1x_)
    : SyncpointManager{host1x_.GetSyncpointManager()} {}

SyncpointManager::SyncpointManager(Tegra::Host1x::SyncpointManager& values)
    : syncpoint_values{values} {
    constexpr u32 VBlank0SyncpointId{26};
    constexpr u32 VBlank1SyncpointId{27};

    // Reserve both vblank syncpoints as client managed as they use Continuous Mode
    // Refer to section 14.3.5.3 of the TRM for more information on Continuous Mode
    // https://github.com/Jetson-TX1-AndroidTV/android_kernel_jetson_tx1_hdmi_primary/blob/8f74a72394efb871cb3f886a3de2998cd7ff2990/drivers/gpu/host1x/drm/dc.c#L660
    ReserveSyncpoint(VBlank0SyncpointId, true);
    ReserveSyncpoint(VBlank1SyncpointId, true);

    for (u32 syncpoint_id : channel_syncpoints) {
        if (syncpoint_id) {
            ReserveSyncpoint(syncpoint_id, false);
        }
    }
    // Host1x can outlive NvCore. Import its sticky quarantine before ordinary allocations.
    for (u32 id = 1; id < syncpoints.size(); ++id) {
        if (!syncpoints[id].reserved && syncpoint_values.UsesCpuGpuCompletion(id)) {
            ReserveSyncpoint(id, false);
        }
    }
}

SyncpointManager::~SyncpointManager() = default;

u32 SyncpointManager::ReserveSyncpoint(u32 id, bool client_managed) {
    auto& syncpoint = syncpoints.at(id);

    if (syncpoint.reserved) {
        ASSERT_MSG(false, "Requested syncpoint is in use");
        return 0;
    }

    syncpoint.reserved = true;
    syncpoint_values.NoteSyncpointReservation(id);
    syncpoint.interface_managed = client_managed;

    return id;
}

u32 SyncpointManager::FindFreeSyncpoint() {
    for (u32 i{1}; i < syncpoints.size(); i++) {
        if (!syncpoints[i].reserved) {
            return i;
        }
    }
    ASSERT_MSG(false, "Failed to find a free syncpoint!");
    return 0;
}

u32 SyncpointManager::AllocateSyncpoint(bool client_managed) {
    std::lock_guard lock(reservation_lock);
    return ReserveSyncpoint(FindFreeSyncpoint(), client_managed);
}

std::optional<u32> SyncpointManager::TryAllocateSyncpoint(bool client_managed) {
    std::lock_guard lock(reservation_lock);
    for (u32 id = 1; id < syncpoints.size(); ++id) {
        if (!syncpoints[id].reserved) return ReserveSyncpoint(id, client_managed);
    }
    return std::nullopt;
}

std::optional<u32> SyncpointManager::TryAllocateGpuCompletionSyncpoint() {
    std::lock_guard lock(reservation_lock);
    for (u32 id = 1; id < syncpoints.size(); ++id) {
        if (!syncpoints[id].reserved && !syncpoint_values.WasSyncpointReserved(id) &&
            syncpoint_values.MarkCpuGpuCompletion(id)) {
            return ReserveSyncpoint(id, false);
        }
    }
    return std::nullopt;
}

void SyncpointManager::FreeSyncpoint(u32 id) {
    std::lock_guard lock(reservation_lock);
    auto& syncpoint = syncpoints.at(id);
    ASSERT(syncpoint.reserved);
    if (syncpoint_values.UsesCpuGpuCompletion(id)) {
        // A late real GPU callback or old SDK waiter must never act on a reused ID.
        // Deliberately retained until NvCore shutdown, including unfinished counters.
        LOG_INFO(Service_NVDRV, "NX2 CPU completion: quarantine syncpoint {} guest={} host={}",
                 id, syncpoint_values.GetGuestSyncpointValue(id),
                 syncpoint_values.GetHostSyncpointValue(id));
        return;
    }
    syncpoint.reserved = false;
}

bool SyncpointManager::IsSyncpointAllocated(u32 id) const {
    return (id < SyncpointCount) && syncpoints[id].reserved;
}

bool SyncpointManager::HasSyncpointExpired(u32 id, u32 threshold) const {
    const SyncpointInfo& syncpoint{syncpoints.at(id)};

    if (!syncpoint.reserved) {
        ASSERT(false);
        return false;
    }

    // If the interface manages counters then we don't keep track of the maximum value as it handles
    // sanity checking the values then
    if (syncpoint.interface_managed) {
        return static_cast<s32>(syncpoint.counter_min - threshold) >= 0;
    } else {
        return (syncpoint.counter_max - threshold) >= (syncpoint.counter_min - threshold);
    }
}

u32 SyncpointManager::IncrementSyncpointMaxExt(u32 id, u32 amount) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    return syncpoint.counter_max += amount;
}

u32 SyncpointManager::ReadSyncpointMinValue(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    return syncpoint.counter_min;
}

u32 SyncpointManager::UpdateMin(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    syncpoint.counter_min = syncpoint_values.GetHostSyncpointValue(id);
    return syncpoint.counter_min;
}

NvFence SyncpointManager::GetSyncpointFence(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return NvFence{};
    }

    return {
        .id = static_cast<s32>(id),
        .value = syncpoint.counter_max,
    };
}

} // namespace Service::Nvidia::NvCore
