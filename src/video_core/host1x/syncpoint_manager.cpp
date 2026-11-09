// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "common/logging.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

namespace {

// EDEN_NX2_SYNCPOINT_FAST=0: Increment always takes the lock and walks the syncpoint's actions,
// even with none registered and nobody waiting on it.
bool IncrementFastPath() {
    static const bool fast = [] {
        const char* value = std::getenv("EDEN_NX2_SYNCPOINT_FAST");
        return value == nullptr || value[0] != '0';
    }();
    return fast;
}

} // Anonymous namespace

namespace {

u64 SteadyNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

u32 ThreadTag() {
#if defined(__linux__)
    return static_cast<u32>(gettid());
#else
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

} // Anonymous namespace

void SyncpointManager::Nx2NoteIncrement(u32 syncpoint_id, u32 value, bool host) {
    const char tag = VideoCommon::GpuSplit::OnBackThread()  ? 'B'
                     : VideoCommon::GpuSplit::OnGpuThread() ? 'G'
                                                            : 'C';
    auto& entry = nx2_increments[nx2_increment_count.fetch_add(1, std::memory_order_relaxed) %
                                 nx2_increments.size()];
    entry.word.store(u64{value} | (u64{syncpoint_id} << 32) | (u64{host} << 40) |
                         (u64{static_cast<u8>(tag)} << 48),
                     std::memory_order_relaxed);
    entry.ns.store(SteadyNs(), std::memory_order_relaxed);
    entry.thread.store(ThreadTag(), std::memory_order_relaxed);
}

std::string SyncpointManager::Nx2DebugState() {
    std::string state;
    {
        std::unique_lock lk{guard, std::try_to_lock};
        const auto thresholds = [](const std::list<RegisteredAction>& actions) {
            std::string text;
            size_t shown = 0;
            for (const auto& action : actions) {
                if (shown++ == 3) {
                    text += ",..";
                    break;
                }
                text += fmt::format("{}{}", shown == 1 ? "" : ",", action.expected_value);
            }
            return text;
        };
        for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
            const u32 guest = syncpoints_guest[id].load(std::memory_order_relaxed);
            const u32 host = syncpoints_host[id].load(std::memory_order_relaxed);
            const u32 guest_actions = pending_guest[id].load(std::memory_order_relaxed);
            const u32 host_actions = pending_host[id].load(std::memory_order_relaxed);
            if (guest == 0 && host == 0 && guest_actions == 0 && host_actions == 0) {
                continue;
            }
            state += fmt::format(" {}:g{}/h{}", id, guest, host);
            if (lk.owns_lock()) {
                if (!guest_action_storage[id].empty()) {
                    state += fmt::format(" waits g[{}]", thresholds(guest_action_storage[id]));
                }
                if (!host_action_storage[id].empty()) {
                    state += fmt::format(" waits h[{}]", thresholds(host_action_storage[id]));
                }
            } else if (guest_actions != 0 || host_actions != 0) {
                state += fmt::format(" ({} guest, {} host actions)", guest_actions, host_actions);
            }
        }
        if (!lk.owns_lock()) {
            state += " (lock busy)";
        }
    }
    state += " | last increments (syncpoint=value@thread, ms ago):";
    const u32 count = nx2_increment_count.load(std::memory_order_relaxed);
    const u64 now = SteadyNs();
    for (u32 i = count > 24 ? count - 24 : 0; i < count; ++i) {
        const auto& entry = nx2_increments[i % nx2_increments.size()];
        const u64 word = entry.word.load(std::memory_order_relaxed);
        state += fmt::format(" {}{}={}@{}{} {:.0f}", (word >> 40) & 1 ? 'h' : 'g',
                             (word >> 32) & 0xff, static_cast<u32>(word),
                             static_cast<char>((word >> 48) & 0xff),
                             entry.thread.load(std::memory_order_relaxed),
                             static_cast<double>(now - entry.ns.load(std::memory_order_relaxed)) /
                                 1e6);
    }
    return state;
}

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
    std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
    std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::scoped_lock lk(guard);
    // Counted before the value is checked again, both seq_cst as in Increment: an Increment that
    // still sees no pending action stored its value first, and the check below sees it.
    pending.fetch_add(1, std::memory_order_seq_cst);
    if (syncpoint.load(std::memory_order_seq_cst) >= expected_value) {
        pending.fetch_sub(1, std::memory_order_relaxed);
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

void SyncpointManager::DeregisterAction(std::atomic<u32>& pending,
                                        std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    std::scoped_lock lk(guard);

    // We want to ensure the iterator still exists prior to erasing it
    // Otherwise, if an invalid iterator was passed in then it could lead to UB
    // It is important to avoid UB in that case since the deregister isn't called from a locked
    // context
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it == handle) {
            action_storage.erase(it);
            pending.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    DeregisterAction(pending_guest[syncpoint_id], guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    DeregisterAction(pending_host[syncpoint_id], host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], pending_guest[syncpoint_id], wait_guest_cv,
              guest_waiters, guest_action_storage[syncpoint_id]);
    Nx2NoteIncrement(syncpoint_id, syncpoints_guest[syncpoint_id].load(std::memory_order_relaxed),
                     false);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], pending_host[syncpoint_id], wait_host_cv,
              host_waiters, host_action_storage[syncpoint_id]);
    Nx2NoteIncrement(syncpoint_id, syncpoints_host[syncpoint_id].load(std::memory_order_relaxed),
                     true);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_guest[syncpoint_id], pending_guest[syncpoint_id], wait_guest_cv, guest_waiters,
         expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    if (!IsValid(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_host[syncpoint_id], pending_host[syncpoint_id], wait_host_cv, host_waiters,
         expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
                                 std::condition_variable& wait_cv, u32& waiters,
                                 std::list<RegisteredAction>& action_storage) {
    const auto new_value{syncpoint.fetch_add(1, std::memory_order_seq_cst) + 1};
    // No action registered on this syncpoint and nobody waiting on it: nothing to run or wake,
    // so skip the lock. RegisterAction and Wait count themselves before they check the value
    // again (seq_cst both sides): either this sees the count, or they see the new value.
    if (pending.load(std::memory_order_seq_cst) == 0 && IncrementFastPath()) {
        return;
    }

    std::scoped_lock lk(guard);
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
        pending.fetch_sub(1, std::memory_order_relaxed);
    }
    // Only with someone waiting: a notify is a system call even without waiters (bionic tracks
    // none), and syncpoints are incremented thousands of times a second (7.6% of the GPU back
    // thread in Mario Kart World on Android). Waiters count themselves under `guard`.
    if (waiters != 0) {
        wait_cv.notify_all();
    }
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
                            std::condition_variable& wait_cv, u32& waiters,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_seq_cst) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    ++waiters;
    // Counted before wait() checks the value again under the lock (see Increment).
    pending.fetch_add(1, std::memory_order_seq_cst);
    wait_cv.wait(lk, pred);
    pending.fetch_sub(1, std::memory_order_relaxed);
    --waiters;
}

} // namespace Host1x

} // namespace Tegra
