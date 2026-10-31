// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <map>

#include "common/common_types.h"

namespace Core::Memory {

/// Guest pages whose next read must take the slow path because the GPU wrote them, each tagged
/// with the generation it was armed in. Callers serialize every member except Generation() and
/// Empty(), which readers check without the lock.
class ReadTraps {
public:
    u64 Generation() const {
        return generation.load(std::memory_order_acquire);
    }

    bool Empty() const {
        return count.load(std::memory_order_relaxed) == 0;
    }

    /// Opens a new generation: a read that started before it cannot disarm what is armed in it.
    u64 NewGeneration() {
        return generation.fetch_add(1, std::memory_order_release) + 1;
    }

    /// Arms the page in this generation, or moves an armed page up to it.
    void Arm(u64 page, u64 page_generation) {
        if (pages.insert_or_assign(page, page_generation).second) {
            count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    template <typename Func>
    void ForEach(u64 page_begin, u64 page_end, Func&& func) const {
        for (auto it = pages.lower_bound(page_begin); it != pages.end() && it->first < page_end;
             ++it) {
            func(it->first);
        }
    }

    /// Disarms the pages in [page_begin, page_end) armed up to flushed_generation, calling func
    /// for each. Pages armed later stay armed.
    template <typename Func>
    void Disarm(u64 page_begin, u64 page_end, u64 flushed_generation, Func&& func) {
        auto it = pages.lower_bound(page_begin);
        while (it != pages.end() && it->first < page_end) {
            if (it->second > flushed_generation) {
                ++it;
                continue;
            }
            func(it->first);
            it = pages.erase(it);
            count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /// Forgets the traps in [page_begin, page_end), leaving page protections to the caller.
    void Drop(u64 page_begin, u64 page_end) {
        Disarm(page_begin, page_end, ~0ULL, [](u64) {});
        NewGeneration();
    }

    /// Forgets every trap. Generations keep counting, so older snapshots stay older.
    void Clear() {
        pages.clear();
        count.store(0, std::memory_order_relaxed);
        NewGeneration();
    }

private:
    std::map<u64, u64> pages;
    std::atomic<size_t> count{};
    std::atomic<u64> generation{};
};

} // namespace Core::Memory
