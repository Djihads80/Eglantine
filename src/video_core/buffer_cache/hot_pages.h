// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EDEN_NX2_HOT_PAGES=1 (off by default): pages the guest CPU rewrites almost every frame stay
// unprotected instead of being write-protected after each upload and faulting again on the next
// write (Mario Kart World: ~6k protects/s on the GPU back thread, ~5k write faults/s on the CPU
// cores). The buffer cache treats such a "hot" page as always CPU-dirty: every synchronization of
// a range holding it compares the guest page with the bytes the GPU last got (a shadow copy) and
// uploads it from the shadow when they differ. A hot page is never assumed clean.
// - A page turns hot when its CPU write faulted in at least EDEN_NX2_HOT_PAGES_MIN (1-4, default
//   3) of the last 4 frames, and goes back to write tracking after 4 frames without a change.
// - Pages the GPU writes (storage buffers, copies, transform feedback), Switch 2 GPU-written and
//   command list pages never turn hot.
// - EDEN_NX2_HOT_PAGES_MAX caps the hot pages (default 4096, 16 MiB of shadow copies).

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::HotPages {

[[nodiscard]] inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_HOT_PAGES");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// Frames of the last 4 a page's CPU writes must have faulted in for it to turn hot.
[[nodiscard]] inline u32 MinFrames() noexcept {
    static const u32 frames = [] {
        const char* const value = std::getenv("EDEN_NX2_HOT_PAGES_MIN");
        const long parsed = value != nullptr ? std::strtol(value, nullptr, 10) : 3;
        return static_cast<u32>(parsed < 1 ? 1 : (parsed > 4 ? 4 : parsed));
    }();
    return frames;
}

[[nodiscard]] inline size_t MaxPages() noexcept {
    static const size_t pages = [] {
        const char* const value = std::getenv("EDEN_NX2_HOT_PAGES_MAX");
        const long long parsed = value != nullptr ? std::strtoll(value, nullptr, 10) : 4096;
        return static_cast<size_t>(parsed < 0 ? 0 : parsed);
    }();
    return pages;
}

/// Pages set in at least `min` of the four history words (bit-sliced count).
[[nodiscard]] inline u64 AtLeast(u32 min, u64 a, u64 b, u64 c, u64 d) noexcept {
    switch (min) {
    case 1:
        return a | b | c | d;
    case 2:
        return (a & b) | (a & c) | (a & d) | (b & c) | (b & d) | (c & d);
    case 3:
        return (a & b & c) | (a & b & d) | (a & c & d) | (b & c & d);
    default:
        return a & b & c & d;
    }
}

/// Counted outside the buffer cache's lock (core memory: protection changes, write faults).
struct SharedStats {
    std::atomic<u64> protects{};      ///< RasterizerMarkRegionCached(cached): mprotect to read-only
    std::atomic<u64> protect_pages{};
    std::atomic<u64> unprotects{};    ///< RasterizerMarkRegionCached(!cached)
    std::atomic<u64> faults{};        ///< guest writes that invalidated GPU-cached memory
};
inline SharedStats g_shared;

inline void NoteProtect(bool cached, u64 size) noexcept {
    if (!Enabled()) {
        return;
    }
    if (cached) {
        g_shared.protects.fetch_add(1, std::memory_order_relaxed);
        g_shared.protect_pages.fetch_add(size >> 12, std::memory_order_relaxed);
    } else {
        g_shared.unprotects.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void NoteFault() noexcept {
    if (Enabled()) {
        g_shared.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Buffer cache side (under its lock).
struct Stats {
    u64 compares{};   ///< hot page synchronizations
    u64 unchanged{};  ///< ... that found the guest page equal to its shadow
    u64 uploads{};    ///< hot pages uploaded (changed, or no shadow yet)
    u64 promoted{};
    u64 demoted{};
    u64 refused{};    ///< candidates that may never turn hot (GPU-written, command lists)
    u64 blocked{};    ///< candidate pages per tick held back by the GPU writing them
    u64 capped{};     ///< candidates over EDEN_NX2_HOT_PAGES_MAX
};

/// Shadow copies of the hot pages: the bytes the GPU copy of each page was last uploaded from.
class ShadowStore {
public:
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr size_t CHUNK_PAGES = 64;

    [[nodiscard]] u8* Acquire(u64 page) {
        const auto [it, inserted] = slots.try_emplace(page, 0U);
        if (inserted) {
            if (free_slots.empty()) {
                const u32 base = static_cast<u32>(chunks.size() * CHUNK_PAGES);
                chunks.push_back(std::make_unique<u8[]>(CHUNK_PAGES * PAGE_SIZE));
                for (u32 i = CHUNK_PAGES; i-- > 0;) {
                    free_slots.push_back(base + i);
                }
            }
            it->second = free_slots.back();
            free_slots.pop_back();
        }
        return SlotPointer(it->second);
    }

    [[nodiscard]] u8* Find(u64 page) const {
        const auto it = slots.find(page);
        return it == slots.end() ? nullptr : SlotPointer(it->second);
    }

    void Release(u64 page) {
        const auto it = slots.find(page);
        if (it != slots.end()) {
            free_slots.push_back(it->second);
            slots.erase(it);
        }
    }

    [[nodiscard]] size_t Size() const noexcept {
        return slots.size();
    }

private:
    [[nodiscard]] u8* SlotPointer(u32 slot) const {
        return chunks[slot / CHUNK_PAGES].get() + (slot % CHUNK_PAGES) * PAGE_SIZE;
    }

    std::unordered_map<u64, u32> slots;
    std::vector<std::unique_ptr<u8[]>> chunks;
    std::vector<u32> free_slots;
};

} // namespace VideoCommon::HotPages
