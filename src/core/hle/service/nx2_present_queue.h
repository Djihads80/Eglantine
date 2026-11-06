// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "common/common_types.h"
#include "core/core_timing.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Service {

/// Releases native display buffers in submission order after their deadline and GPU fence pass.
class Nx2PresentReleaseQueue {
public:
    // The service advertises fifteen buffer slots. Clients must wait for releases before reuse.
    static constexpr size_t Capacity = 15;

    /// Starts an empty queue using an already allocated, session-owned release syncpoint.
    Nx2PresentReleaseQueue(Core::Timing::CoreTiming& timing_,
                           Tegra::Host1x::SyncpointManager& syncpoints_, u32 release_id_)
        : timing{timing_}, syncpoints{syncpoints_}, release_id{release_id_},
          next_value{syncpoints.GetGuestSyncpointValue(release_id)} {
        timer = Core::Timing::CreateEvent(
            "nx2:vi2 buffer release",
            [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                ReleaseReady();
                return std::nullopt;
            });
    }

    /// Stops rescheduling before waiting for the callback, then wakes outstanding release waits.
    /// At most Capacity increments are needed, even if another producer advanced the syncpoint.
    ~Nx2PresentReleaseQueue() {
        {
            std::scoped_lock lock{mutex};
            stopped = true;
        }
        timing.UnscheduleEvent(timer);
        for (const auto& entry : pending) {
            Signal(entry.value);
        }
    }

    /// Enqueues without blocking the present thread; returns no value when all slots are pending.
    std::optional<u32> Queue(std::chrono::nanoseconds deadline, u32 fence_id, u32 fence_value) {
        std::scoped_lock lock{mutex};
        if (stopped || pending.size() == Capacity) {
            return std::nullopt;
        }
        pending.push_back({deadline, fence_id, fence_value, ++next_value});
        if (pending.size() == 1) {
            timing.ScheduleEvent(deadline, timer, true);
        }
        return next_value;
    }

private:
    struct PendingRelease {
        std::chrono::nanoseconds deadline;
        u32 fence_id;
        u32 fence_value;
        u32 value;
    };

    /// Advances once per queued release, without chasing a counter that has already passed it.
    void Signal(u32 value) {
        if (static_cast<s32>(value - syncpoints.GetGuestSyncpointValue(release_id)) > 0) {
            syncpoints.IncrementGuest(release_id);
        }
    }

    /// Polls only the oldest release; GPU waits never block the timing or presentation thread.
    void ReleaseReady() {
        std::scoped_lock lock{mutex};
        if (stopped) {
            return;
        }
        while (!pending.empty()) {
            const auto& entry = pending.front();
            const auto now = timing.GetGlobalTimeNs();
            if (now < entry.deadline) {
                timing.ScheduleEvent(entry.deadline, timer, true);
                return;
            }
            if (entry.fence_id < Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS &&
                !syncpoints.IsReadyGuest(entry.fence_id, entry.fence_value)) {
                timing.ScheduleEvent(std::chrono::microseconds{100}, timer);
                return;
            }
            Signal(entry.value);
            pending.pop_front();
        }
    }

    Core::Timing::CoreTiming& timing;
    Tegra::Host1x::SyncpointManager& syncpoints;
    const u32 release_id;
    u32 next_value;
    std::mutex mutex;
    bool stopped{};
    std::deque<PendingRelease> pending;
    std::shared_ptr<Core::Timing::EventType> timer;
};

} // namespace Service
