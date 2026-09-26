// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

class InvalidationMutex {
public:
    void lock() {
        mutex.lock();
        Acquired();
    }

    bool try_lock() {
        if (!mutex.try_lock()) {
            return false;
        }
        Acquired();
        return true;
    }

    void unlock() {
        --depth;
        mutex.unlock();
    }

    void Defer(DAddr addr, u64 size) {
        std::scoped_lock lock{guard};
        pending.emplace_back(addr, size);
        has_pending.store(true, std::memory_order_release);
    }

    std::function<void(DAddr, u64)> apply;

private:
    void Acquired() {
        if (++depth != 1 || !has_pending.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::scoped_lock lock{guard};
            ready.swap(pending);
            has_pending.store(false, std::memory_order_relaxed);
        }
        for (const auto& [addr, size] : ready) {
            apply(addr, size);
        }
        ready.clear();
    }

    std::recursive_mutex mutex;
    std::mutex guard;
    std::vector<std::pair<DAddr, u64>> pending;
    std::vector<std::pair<DAddr, u64>> ready;
    std::atomic_bool has_pending{};
    u32 depth{};
};

}
