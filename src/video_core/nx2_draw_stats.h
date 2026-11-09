// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2DrawStats {

// EDEN_NX2_BIND_STATS=1 (diagnostic): per-draw costs of the buffer cache and draw preparation
// lookups the optimisation plan weighs. Every 20,000 draws it logs, for each probe, the calls
// per draw, the share that "hit" (what a hit means is named per probe), the time per call and
// the time per draw.

enum class Site : u32 {
    FindBufferIndex,   ///< hit: an existing buffer covered the range (no CreateBuffer)
    FindBufferVertex,
    FindBufferUniform,
    FindBufferStorage,
    FindBufferTexture,
    FindBufferOther,
    HandleRead,        ///< a texture/sampler handle word read; hit: same word as the last read of it
    SamplerLookup,     ///< GetSamplerId; hit: the descriptor was unchanged
    ImageViewLookup,   ///< VisitImageView; hit: the descriptor was unchanged
    UniformSmall,      ///< uniform bind <= 4 KiB; hit: same bytes as this slot's previous bind
    UniformMedium,     ///< 4 KiB < size < 64 KiB
    UniformBank,       ///< 64 KiB native bank
    StageBindUniform,  ///< BindHostGraphicsUniformBuffers; hit: the stage's bindings equal the last draw's
    StageBindStorage,
    StageBindTexture,
    LockPrepareDraw,   ///< lock acquisition; hit: acquired without waiting
    LockInlineMemory,
    LockInvalidation,
    LockCpuWrite,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Site::Count)> SiteNames{
    "FindBuffer (index)",     "FindBuffer (vertex)",  "FindBuffer (uniform)",
    "FindBuffer (storage)",   "FindBuffer (texture)", "FindBuffer (other)",
    "texture handle read",    "GetSamplerId",         "VisitImageView",
    "uniform bind <= 4 KiB",  "uniform bind 4-64 KiB", "uniform bind 64 KiB bank",
    "stage bind: uniform",    "stage bind: storage",  "stage bind: texture",
    "lock: PrepareDraw",      "lock: inline memory",  "lock: invalidation",
    "lock: CPU write fault"};

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_BIND_STATS") != nullptr;
    return enabled;
}

/// EDEN_NX2_BIND_STATS=hash also compares uniform buffer contents with the slot's previous bind
/// (which costs time: use it for the hit rates only, not the timings).
[[nodiscard]] inline bool HashContents() {
    static const bool hash = [] {
        const char* value = std::getenv("EDEN_NX2_BIND_STATS");
        return value != nullptr && value[0] == 'h';
    }();
    return hash;
}

struct Probe {
    std::atomic<u64> calls{0};
    std::atomic<u64> hits{0};
    std::atomic<u64> cycles{0};
};

inline std::array<Probe, static_cast<size_t>(Site::Count)> g_probes{};
inline std::atomic<u64> g_draws{0};

[[nodiscard]] inline u64 Cycles() {
    return __builtin_ia32_rdtsc();
}

/// Which FindBuffer probe the calling Update* function is feeding.
inline thread_local Site t_find_site = Site::FindBufferOther;

class FindCaller {
public:
    explicit FindCaller(Site site) : previous{t_find_site} {
        t_find_site = site;
    }
    ~FindCaller() {
        t_find_site = previous;
    }
    FindCaller(const FindCaller&) = delete;
    FindCaller& operator=(const FindCaller&) = delete;

private:
    Site previous;
};

/// Times a scope and counts a hit. Free when the statistics are off.
class Timer {
public:
    explicit Timer(Site site_) : site{site_}, enabled{Enabled()} {
        if (enabled) {
            start = Cycles();
        }
    }
    ~Timer() {
        if (!enabled) {
            return;
        }
        Probe& probe = g_probes[static_cast<size_t>(site)];
        probe.calls.fetch_add(1, std::memory_order_relaxed);
        probe.cycles.fetch_add(Cycles() - start, std::memory_order_relaxed);
        if (hit) {
            probe.hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    bool hit{};

private:
    Site site;
    bool enabled;
    u64 start{};
};

/// A lock acquisition: hit means it took under ~0.3 us (nobody held it).
class LockTimer {
public:
    explicit LockTimer(Site site_) : site{site_}, enabled{Enabled()} {
        if (enabled) {
            start = Cycles();
        }
    }
    /// Call right after the lock was taken.
    void Acquired() {
        if (!enabled) {
            return;
        }
        const u64 waited = Cycles() - start;
        Probe& probe = g_probes[static_cast<size_t>(site)];
        probe.calls.fetch_add(1, std::memory_order_relaxed);
        probe.cycles.fetch_add(waited, std::memory_order_relaxed);
        if (waited < 1500) {
            probe.hits.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    Site site;
    bool enabled;
    u64 start{};
};

/// A call that only counts (no timing).
inline void Note(Site site, bool hit) {
    if (!Enabled()) {
        return;
    }
    Probe& probe = g_probes[static_cast<size_t>(site)];
    probe.calls.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
        probe.hits.fetch_add(1, std::memory_order_relaxed);
    }
}

/// A 64-bit value per key, to ask "same as last time?" (direct mapped, collisions count as a miss).
[[nodiscard]] inline bool SameAsLast(u64 key, u64 value) {
    struct Entry {
        u64 key;
        u64 value;
    };
    static thread_local std::array<Entry, 8192> table{};
    Entry& entry = table[(key ^ (key >> 13) ^ (key >> 29)) % table.size()];
    const bool same = entry.key == key && entry.value == value;
    entry = Entry{key, value};
    return same;
}

[[nodiscard]] inline u64 HashBytes(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ULL;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 word;
        __builtin_memcpy(&word, data + i, 8);
        hash = (hash ^ word) * 0x100000001b3ULL;
    }
    for (; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x100000001b3ULL;
    }
    return hash;
}

/// Called once per draw: logs and restarts the window every 20,000 draws.
inline void OnDraw() {
    if (!Enabled()) {
        return;
    }
    static const auto first_time = std::chrono::steady_clock::now();
    static const u64 first_cycles = Cycles();
    static std::atomic<u64> window_cycles{0};
    static std::atomic<std::chrono::steady_clock::rep> window_time{0};
    const u64 draws = g_draws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (draws % 20000 != 0) {
        return;
    }
    const double elapsed_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             first_time)
            .count());
    const double cycles_per_ns = static_cast<double>(Cycles() - first_cycles) / elapsed_ns;
    std::string text;
    for (size_t i = 0; i < g_probes.size(); ++i) {
        Probe& probe = g_probes[i];
        const u64 calls = probe.calls.exchange(0);
        const u64 hits = probe.hits.exchange(0);
        const u64 cycles = probe.cycles.exchange(0);
        if (calls == 0) {
            continue;
        }
        text += fmt::format("\n  {:<26} {:6.2f}/draw  hit {:5.1f}%  {:6.0f} ns/call  {:6.2f} us/draw",
                            SiteNames[i], static_cast<double>(calls) / 20000.0,
                            100.0 * static_cast<double>(hits) / static_cast<double>(calls),
                            static_cast<double>(cycles) / static_cast<double>(calls) / cycles_per_ns,
                            static_cast<double>(cycles) / cycles_per_ns / 1000.0 / 20000.0);
    }
    LOG_INFO(HW_GPU, "nx2 draw probes (20000 draws):{}", text);
}

} // namespace VideoCommon::Nx2DrawStats
