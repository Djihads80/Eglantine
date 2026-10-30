// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <type_traits>

#include "common/common_types.h"
#include "common/thread_worker.h"

namespace Tegra::Texture {

Common::ThreadWorker& GetThreadWorkers();

/// Runs body(0) .. body(count - 1) on the calling thread and on the transcode workers that are
/// free, and returns once all have run. The calling thread takes whatever indices no worker has
/// started, so a pool busy with other work (asynchronous decodes) never makes this slower than a
/// plain loop. The workers' writes are visible to the caller when it returns.
template <typename Body>
void ParallelFor(u32 count, Body&& body) {
    if (count <= 1) {
        if (count == 1) {
            body(0);
        }
        return;
    }
    struct State {
        std::atomic<u32> next{0};
        std::atomic<u32> done{0};
        u32 count{};
        void* context{};
        void (*call)(void*, u32){};
    };
    const auto run = [](State& state) {
        for (u32 index = state.next.fetch_add(1, std::memory_order_relaxed); index < state.count;
             index = state.next.fetch_add(1, std::memory_order_relaxed)) {
            state.call(state.context, index);
            if (state.done.fetch_add(1, std::memory_order_acq_rel) + 1 == state.count) {
                state.done.notify_all();
            }
        }
    };
    // Shared: a worker that gets to its task after the caller returned finds no index left and
    // only touches the counters.
    const auto state = std::make_shared<State>();
    state->count = count;
    state->context = std::addressof(body);
    state->call = [](void* context, u32 index) {
        (*static_cast<std::remove_reference_t<Body>*>(context))(index);
    };
    Common::ThreadWorker& workers = GetThreadWorkers();
    const u32 helpers = (std::min)(count - 1, 8U);
    for (u32 i = 0; i < helpers; ++i) {
        workers.QueueWork([state, run] { run(*state); });
    }
    run(*state);
    for (u32 done = state->done.load(std::memory_order_acquire); done != count;
         done = state->done.load(std::memory_order_acquire)) {
        state->done.wait(done, std::memory_order_acquire);
    }
}

} // namespace Tegra::Texture
