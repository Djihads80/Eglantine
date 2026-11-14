// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/common_types.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace Common {

/// A spin-wait hint for the current core.
inline void CpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

/// A monotonic completion position shared by producers of progress and any number of waiters:
/// ring positions consumed by a worker, fences signalled, ticks submitted.
///
/// Contract:
/// - Producers only move the value forward (Advance). A producer that publishes in batches must
///   still publish every position it reaches before it goes idle; LowestTarget() only lets it
///   publish earlier for a waiter, it is a hint and may be stale.
/// - Waiters spin for a short budget (most waits are a few microseconds), then block. A producer
///   only takes the lock when a waiter is blocked, so an Advance nobody waits for is a store and
///   a load.
/// - Cancel() ends every wait whose target is not reached (shutdown, a worker that stopped); it
///   stays in effect until Reset().
class ProgressCounter {
public:
    enum class WaitResult {
        Reached,   ///< The value reached the target
        Cancelled, ///< Cancel() was called (or the abort predicate held) before it did
        TimedOut,  ///< The deadline passed before it did
    };

    static constexpr u64 NoTarget = ~u64{0};
    /// Default time a waiter spins before it blocks.
    static constexpr std::chrono::microseconds DefaultSpin{20};

    explicit ProgressCounter(u64 initial = 0) : value{initial} {}

    ProgressCounter(const ProgressCounter&) = delete;
    ProgressCounter& operator=(const ProgressCounter&) = delete;

    [[nodiscard]] u64 Value() const noexcept {
        return value.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Reached(u64 target) const noexcept {
        return value.load(std::memory_order_acquire) >= target;
    }

    [[nodiscard]] bool IsCancelled() const noexcept {
        return cancelled.load(std::memory_order_acquire);
    }

    /// The lowest target a waiter currently waits for (NoTarget when none), a hint.
    [[nodiscard]] u64 LowestTarget() const noexcept {
        return lowest_target.load(std::memory_order_relaxed);
    }

    /// Whether a waiter is blocked (not just spinning).
    [[nodiscard]] bool HasSleepers() const noexcept {
        return sleepers.load(std::memory_order_relaxed) != 0;
    }

    /// Single producer: publishes `new_value` (not below the current value).
    void Advance(u64 new_value) noexcept {
        value.store(new_value, std::memory_order_seq_cst);
        NotifyIfSleeping();
    }

    /// Single producer: makes `new_value` visible to spinning waiters and to blocked ones at
    /// their next poll, without waking anyone (for a producer that publishes often and wakes
    /// with Advance where LowestTarget() says a waiter wants the position).
    void Publish(u64 new_value) noexcept {
        value.store(new_value, std::memory_order_release);
    }

    /// Any number of producers: moves the value to at least `new_value`.
    void AdvanceAtLeast(u64 new_value) noexcept {
        u64 current = value.load(std::memory_order_relaxed);
        while (current < new_value &&
               !value.compare_exchange_weak(current, new_value, std::memory_order_seq_cst,
                                            std::memory_order_relaxed)) {
        }
        NotifyIfSleeping();
    }

    /// Any number of producers: adds `amount` and returns the new value.
    u64 Increment(u64 amount = 1) noexcept {
        const u64 result = value.fetch_add(amount, std::memory_order_seq_cst) + amount;
        NotifyIfSleeping();
        return result;
    }

    /// Ends all current and future waits for unreached targets until Reset().
    void Cancel() noexcept {
        cancelled.store(true, std::memory_order_seq_cst);
        std::scoped_lock lk{mutex};
        cv.notify_all();
    }

    /// Starts over at `new_value`, not cancelled. No waiter may be waiting.
    void Reset(u64 new_value = 0) noexcept {
        value.store(new_value, std::memory_order_seq_cst);
        cancelled.store(false, std::memory_order_seq_cst);
        lowest_target.store(NoTarget, std::memory_order_relaxed);
    }

    /// Waits until the value reaches `target` or the counter is cancelled.
    WaitResult WaitFor(u64 target, std::chrono::nanoseconds spin = DefaultSpin) {
        return Wait(target, spin, std::chrono::steady_clock::time_point::max(), [] { return false; },
                    std::chrono::milliseconds{1});
    }

    /// Waits until the value reaches `target`, the counter is cancelled or `deadline` passes.
    WaitResult WaitUntil(u64 target, std::chrono::steady_clock::time_point deadline,
                         std::chrono::nanoseconds spin = DefaultSpin) {
        return Wait(target, spin, deadline, [] { return false; }, std::chrono::milliseconds{1});
    }

    /// Like WaitUntil, also ending (Cancelled) once `abort()` returns true; `abort` is checked
    /// at least every `poll` while blocked.
    template <typename Abort>
    WaitResult Wait(u64 target, std::chrono::nanoseconds spin,
                    std::chrono::steady_clock::time_point deadline, Abort&& abort,
                    std::chrono::nanoseconds poll) {
        if (Reached(target)) {
            return WaitResult::Reached;
        }
        waiters.fetch_add(1, std::memory_order_relaxed);
        RequestTarget(target);
        // Spin phase: no syscalls, for the waits the producer ends within microseconds.
        if (spin.count() > 0) {
            const auto spin_end = std::chrono::steady_clock::now() + spin;
            for (u32 i = 1;; ++i) {
                if (Reached(target)) {
                    return Finish(WaitResult::Reached);
                }
                if (IsCancelled()) {
                    return Finish(WaitResult::Cancelled);
                }
                CpuRelax();
                if ((i & 63) == 0) {
                    RequestTarget(target);
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= spin_end || now >= deadline) {
                        break;
                    }
                    if (abort()) {
                        return Finish(Reached(target) ? WaitResult::Reached : WaitResult::Cancelled);
                    }
                }
            }
        }
        // Blocking phase.
        std::unique_lock lk{mutex};
        sleepers.fetch_add(1, std::memory_order_seq_cst);
        blocks.fetch_add(1, std::memory_order_relaxed);
        WaitResult result = WaitResult::Reached;
        for (;;) {
            // seq_cst: pairs with the producer's store then sleepers load (NotifyIfSleeping).
            if (value.load(std::memory_order_seq_cst) >= target) {
                result = WaitResult::Reached;
                break;
            }
            if (cancelled.load(std::memory_order_seq_cst)) {
                result = WaitResult::Cancelled;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                result = WaitResult::TimedOut;
                break;
            }
            lk.unlock();
            const bool aborted = abort();
            lk.lock();
            if (aborted) {
                result = value.load(std::memory_order_seq_cst) >= target ? WaitResult::Reached
                                                                          : WaitResult::Cancelled;
                break;
            }
            // The hint may have been reset by another waiter that finished: assert it again.
            RequestTarget(target);
            const auto slice_end = (deadline - now) > poll ? now + poll : deadline;
            cv.wait_until(lk, slice_end, [&] {
                return value.load(std::memory_order_seq_cst) >= target ||
                       cancelled.load(std::memory_order_seq_cst);
            });
        }
        sleepers.fetch_sub(1, std::memory_order_relaxed);
        lk.unlock();
        return Finish(result);
    }

    /// Diagnostics: waits that had to block.
    [[nodiscard]] u64 BlockCount() const noexcept {
        return blocks.load(std::memory_order_relaxed);
    }

private:
    void NotifyIfSleeping() noexcept {
        if (sleepers.load(std::memory_order_seq_cst) != 0) {
            // Taking the lock orders this notify after a sleeper's last check of the value.
            { std::scoped_lock lk{mutex}; }
            cv.notify_all();
        }
    }

    void RequestTarget(u64 target) noexcept {
        u64 current = lowest_target.load(std::memory_order_relaxed);
        while (target < current &&
               !lowest_target.compare_exchange_weak(current, target, std::memory_order_seq_cst,
                                                    std::memory_order_relaxed)) {
        }
    }

    WaitResult Finish(WaitResult result) noexcept {
        // With nobody else waiting the hint goes away; a waiter that starts meanwhile asserts it
        // again before it blocks and on every poll.
        if (waiters.fetch_sub(1, std::memory_order_relaxed) == 1) {
            lowest_target.store(NoTarget, std::memory_order_relaxed);
        }
        return result;
    }

    alignas(64) std::atomic<u64> value;
    alignas(64) std::atomic<u32> sleepers{};
    std::atomic<u32> waiters{}; ///< Spinning or blocked
    std::atomic<bool> cancelled{};
    std::atomic<u64> lowest_target{NoTarget};
    std::atomic<u64> blocks{};
    std::mutex mutex;
    std::condition_variable cv;
};

} // namespace Common
