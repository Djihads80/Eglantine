// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

bool SyncpointManager::IsValid(u32 id) {
    if (id < NUM_MAX_SYNCPOINTS) [[likely]] {
        return true;
    }
    // Reported a few times: a bad id is usually a misread register, and it repeats every frame.
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_ERROR(HW_GPU, "Syncpoint id {} out of range (max {}), ignored", id, NUM_MAX_SYNCPOINTS);
    }
    return false;
}

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::scoped_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, std::move(action));
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    std::scoped_lock lk(guard);

    // We want to ensure the iterator still exists prior to erasing it
    // Otherwise, if an invalid iterator was passed in then it could lead to UB
    // It is important to avoid UB in that case since the deregister isn't called from a locked
    // context
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_waiters,
              guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_waiters,
              host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_waiters, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, host_waiters, expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 u32& waiters, std::list<RegisteredAction>& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    std::scoped_lock lk(guard);
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
    }
    // Only with someone waiting: a notify is a system call even without waiters (bionic tracks
    // none), and syncpoints are incremented thousands of times a second (7.6% of the GPU back
    // thread in Mario Kart World on Android). Waiters count themselves under `guard`.
    if (waiters != 0) {
        wait_cv.notify_all();
    }
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32& waiters, u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    ++waiters;
    wait_cv.wait(lk, pred);
    --waiters;
}

} // namespace Host1x

} // namespace Tegra
