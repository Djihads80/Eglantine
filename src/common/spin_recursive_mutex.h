// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Common {

/// A recursive mutex that spins briefly before sleeping. The GPU caches' locks are taken tens of
/// thousands of times a second by the GPU thread, the fencing thread and CPU write traps, and
/// held for microseconds: std::recursive_mutex put a waiting thread to sleep at once, and its
/// wake-up took longer than the critical section (title 1's GPU thread spent ~8% asleep there).
class SpinRecursiveMutex {
public:
    void lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            LockContended();
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }

    bool try_lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return false;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }

    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(std::thread::id{}, std::memory_order_relaxed);
        if (state.exchange(0, std::memory_order_release) == 2) {
            state.notify_one();
        }
    }

private:
    // About 10 microseconds of spinning before sleeping: 1024 `pause` on x86. On ARM64 `yield`
    // is a single cycle: 1024 of them lasted 1-2 us, and a waiter went on to libc++'s atomic
    // wait, which calls sched_yield for up to 64 us before it sleeps (0.6 ms of each Mario
    // Kart World frame on the Odin 3's GPU back thread). There `isb` (tens of cycles) is the
    // pause, 2048 times; EDEN_NX2_MUTEX_SPIN=<n> sets the count.
    static int SpinCount() {
#if defined(__aarch64__)
        static const int spins = [] {
            const char* const value = std::getenv("EDEN_NX2_MUTEX_SPIN");
            const int count = value != nullptr ? std::atoi(value) : 0;
            return count > 0 ? count : 2048;
        }();
        return spins;
#else
        return 1024;
#endif
    }

    static void Pause() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("isb");
#endif
    }

    void LockContended() {
        const int spin_count = SpinCount();
        for (int spin = 0; spin < spin_count; ++spin) {
            Pause();
            if (state.load(std::memory_order_relaxed) == 0) {
                std::uint32_t expected = 0;
                if (state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return;
                }
            }
        }
        // 2: held with (possible) sleepers; taken this way it stays marked, so its unlock wakes
        // the next sleeper.
        while (state.exchange(2, std::memory_order_acquire) != 0) {
            state.wait(2, std::memory_order_relaxed);
        }
    }

    std::atomic<std::uint32_t> state{0}; ///< 0 free, 1 held, 2 held with sleepers
    std::atomic<std::thread::id> owner{};
    std::uint32_t depth{}; ///< Recursion depth, touched only by the owner
};

} // namespace Common
