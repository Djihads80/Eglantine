// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <utility>

#include "common/common_types.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    u32 GetGuestSyncpointValue(u32 id) const {
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    // CPU-only completion domain. It is assigned before the channel can submit,
    // never reset, and its allocator keeps the ID reserved through NvCore shutdown.
    bool MarkCpuGpuCompletion(u32 id) {
        if (id == 0 || id >= NUM_MAX_SYNCPOINTS || WasSyncpointReserved(id) ||
            GetGuestSyncpointValue(id) != 0 ||
            GetHostSyncpointValue(id) != 0) {
            return false;
        }
        bool expected = false;
        return cpu_gpu_completion[id].compare_exchange_strong(expected, true,
                                                              std::memory_order_acq_rel);
    }

    void NoteSyncpointReservation(u32 id) {
        syncpoint_ever_reserved[id].store(true, std::memory_order_release);
    }

    bool WasSyncpointReserved(u32 id) const {
        return syncpoint_ever_reserved[id].load(std::memory_order_acquire);
    }

    bool UsesCpuGpuCompletion(u32 id) const {
        return id < NUM_MAX_SYNCPOINTS &&
               cpu_gpu_completion[id].load(std::memory_order_acquire);
    }

    u32 GetCpuSyncpointValue(u32 id) const {
        return UsesCpuGpuCompletion(id) ? GetHostSyncpointValue(id) : GetGuestSyncpointValue(id);
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
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    struct CpuActionHandle {
        ActionHandle action{};
        bool host{};
    };

    template <typename Func>
    CpuActionHandle RegisterCpuAction(u32 id, u32 value, Func&& action) {
        const bool host = UsesCpuGpuCompletion(id);
        auto handle = host ? RegisterHostAction(id, value, std::forward<Func>(action))
                           : RegisterGuestAction(id, value, std::forward<Func>(action));
        return {handle, host};
    }

    void DeregisterCpuAction(u32 id, const CpuActionHandle& handle) {
        if (handle.host) {
            DeregisterHostAction(id, handle.action);
        } else {
            DeregisterGuestAction(id, handle.action);
        }
    }

    // The enqueue callback receives whether the completion must be a real GPU fence.
    // Guest still advances at command processing, so GPU-side waits are unchanged.
    template <typename Enqueue>
    void QueueGpuSyncpoint(u32 id, Enqueue&& enqueue) {
        IncrementGuest(id);
        enqueue([this, id] { IncrementHost(id); }, UsesCpuGpuCompletion(id));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

private:
    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   std::list<RegisteredAction>& action_storage);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(std::list<RegisteredAction>& action_storage, const ActionHandle& handle);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value);

    std::array<std::atomic<bool>, NUM_MAX_SYNCPOINTS> cpu_gpu_completion{};
    std::array<std::atomic<bool>, NUM_MAX_SYNCPOINTS> syncpoint_ever_reserved{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    std::mutex guard;
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
};

} // namespace Host1x

} // namespace Tegra
