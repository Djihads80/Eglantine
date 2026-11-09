// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"

// EDEN_NX2_SAMPLE=<period in us> (ARM64 Linux and Android; "1" is 1000): a thread looks every
// period at what the GPU threads do: what each says it waits for (the marks below), whether it
// and the guest's cores run or sleep, and the call stack of each while it works. One record per
// look goes to nx2_sample_ticks.bin in the log directory, the stacks to nx2_sample_stacks.txt.
// It answers "what does the back thread wait for" without a profiler on the device.
namespace VideoCommon::Nx2Sampler {

enum class Role : s32 {
    None = -1,
    Back,  ///< The split GPU thread's back half (GPUBack)
    Front, ///< The GPU thread
    Count,
};

/// What a thread waits for. Stacks are taken in Work and BackHook only.
enum Mark : u32 {
    Work = 0,
    BackSpin,            ///< The back is out of work and spins
    BackHook,            ///< The back is out of work and runs its idle hook (deferred submit)
    BackParked,          ///< The back is out of work and sleeps
    FrontIdle,           ///< The front waits for the guest's next command list
    FrontPending,        ///< The front polls channels suspended on a semaphore the guest releases
    FrontOnBack = 0x100, ///< Plus the Nx2Profile::Zone of the wait: the front waits for the back
};

inline std::atomic<bool> g_enabled{};
inline std::array<std::atomic<u32>, static_cast<size_t>(Role::Count)> g_marks{};
inline std::atomic<u32> g_frames{};
constinit inline thread_local Role t_role{Role::None};

/// The current thread takes `role`. The first call starts the sampler if EDEN_NX2_SAMPLE is set.
void Register(Role role);

/// The current thread gives its role up (before it ends).
void Unregister();

/// The current thread holds `role` for the life of the object.
struct Registration {
    explicit Registration(Role role) {
        Register(role);
    }

    ~Registration() {
        Unregister();
    }

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
};

/// A game frame ended.
inline void Frame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

/// The current thread's mark for the life of the scope.
class Scope {
public:
    explicit Scope(u32 mark) {
        if (!g_enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const Role role = t_role;
        if (role == Role::None) {
            return;
        }
        slot = &g_marks[static_cast<size_t>(role)];
        previous = slot->exchange(mark, std::memory_order_relaxed);
    }

    ~Scope() {
        if (slot != nullptr) {
            slot->store(previous, std::memory_order_relaxed);
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::atomic<u32>* slot{};
    u32 previous{};
};

} // namespace VideoCommon::Nx2Sampler
