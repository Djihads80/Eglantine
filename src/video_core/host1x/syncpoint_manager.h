// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>

#include "common/common_types.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    /// Whether `id` names a syncpoint. Ids come from guest registers (16 bits in the 3D engine's
    /// sync_info, 24 in the puller's fence action) and index fixed arrays; out of range ones are
    /// reported and treated as always signalled instead of reaching past them.
    static bool IsValid(u32 id);

    u32 GetGuestSyncpointValue(u32 id) const {
        return IsValid(id) ? syncpoints_guest[id].load(std::memory_order_acquire) : 0;
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return IsValid(id) ? syncpoints_host[id].load(std::memory_order_acquire) : 0;
    }

    struct RegisteredAction {
        explicit RegisteredAction(u32 expected_value_, std::function<void()>&& action_)
            : expected_value{expected_value_}, action{std::move(action_)} {}
        u32 expected_value;
        std::function<void()> action;
    };
    using ActionHandle = std::list<RegisteredAction>::iterator;

    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!IsValid(syncpoint_id)) {
            action();
            return {};
        }
        return RegisterAction(syncpoints_guest[syncpoint_id], pending_guest[syncpoint_id],
                              guest_action_storage[syncpoint_id], expected_value,
                              std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!IsValid(syncpoint_id)) {
            action();
            return {};
        }
        return RegisterAction(syncpoints_host[syncpoint_id], pending_host[syncpoint_id],
                              host_action_storage[syncpoint_id], expected_value,
                              std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return !IsValid(syncpoint_id) ||
               syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return !IsValid(syncpoint_id) ||
               syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

private:
    void Increment(std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
                   std::condition_variable& wait_cv, u32& waiters,
                   std::list<RegisteredAction>& action_storage);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(std::atomic<u32>& pending, std::list<RegisteredAction>& action_storage,
                          const ActionHandle& handle);

    void Wait(std::atomic<u32>& syncpoint, std::atomic<u32>& pending,
              std::condition_variable& wait_cv, u32& waiters, u32 expected_value);

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    /// Per syncpoint: its registered actions plus the threads in Wait on it, changed under
    /// `guard`. Increment takes `guard` only when it is nonzero.
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> pending_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> pending_host{};

    std::mutex guard;
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
    u32 guest_waiters{}; ///< Threads in Wait on wait_guest_cv, under guard
    u32 host_waiters{};  ///< Threads in Wait on wait_host_cv, under guard
};

} // namespace Host1x

} // namespace Tegra
