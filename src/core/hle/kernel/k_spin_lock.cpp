// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_spin_lock.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Kernel {

namespace {

// Spins of about 30-140 cycles each before a contended Lock sleeps (a few microseconds).
constexpr int SpinCount = 128;

void Pause() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

} // Anonymous namespace

void KSpinLock::Lock() {
    u32 expected = 0;
    if (m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
    }
    for (int spin = 0; spin < SpinCount; ++spin) {
        Pause();
        if (m_state.load(std::memory_order_relaxed) == 0) {
            expected = 0;
            if (m_state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return;
            }
        }
    }
    // Mark the lock contended and sleep until it is released; taken this way it stays marked,
    // so its Unlock wakes the next sleeper.
    while (m_state.exchange(2, std::memory_order_acquire) != 0) {
        m_state.wait(2, std::memory_order_relaxed);
    }
}

void KSpinLock::Unlock() {
    if (m_state.exchange(0, std::memory_order_release) == 2) {
        m_state.notify_one();
    }
}

bool KSpinLock::TryLock() {
    u32 expected = 0;
    return m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed);
}

} // namespace Kernel
