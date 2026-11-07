// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

// EDEN_NX2_QUICK_BIND=1 (strato's quick binds, for textures): a draw reuses the image views and
// samplers its pipeline resolved at its previous draw when nothing they came from can have
// changed: the texture and sampler handles it reads from its constant buffers (read again every
// draw and compared), the pools those handles index (the pool generation below), and the texture
// cache's images (TextureCache::Nx2QuickSerial). It skips the pool reads, view and sampler
// lookups and image preparation of GraphicsPipeline::ConfigureImpl. =verify does both and logs
// what differs. Off with the GPU split (its front and back would race on the generation).
//
// The pools are guest memory the GPU only reads. The guest CPU writes them between submissions
// (it can't know when a queued draw reads them): the generation moves at each submission and
// semaphore acquire, and at every engine method that writes memory (DMA copies, inline uploads,
// 2D blits, compute dispatches).
namespace Tegra {

/// 0: off, 1: on, 2: verify.
[[nodiscard]] inline int Nx2QuickBind() {
    static const int mode = [] {
        const char* const value = std::getenv("EDEN_NX2_QUICK_BIND");
        if (value == nullptr) {
            return 0;
        }
        return value[0] == '1' ? 1 : value[0] == 'v' ? 2 : 0;
    }();
    return mode;
}

/// EDEN_NX2_FAST_BUFFERS=1: a draw synchronizes and binds only the vertex buffers its vertex
/// shader reads (the bindings of the attributes it loads), not every enabled stream: Mario Kart
/// World keeps all 32 enabled and synchronized them all for every draw (~1.5 million times a
/// second). Streams it skips keep their dirty flags and are synchronized when a draw reads them.
/// =verify binds them all and counts what would have been skipped. Off with the GPU split.
/// 0: off, 1: on, 2: verify.
[[nodiscard]] inline int Nx2FastBuffers() {
    static const int mode = [] {
        const char* const value = std::getenv("EDEN_NX2_FAST_BUFFERS");
        if (value == nullptr) {
            return 0;
        }
        return value[0] == '1' ? 1 : value[0] == 'v' ? 2 : 0;
    }();
    return mode;
}

inline std::atomic<u64> g_nx2_pool_generation{1};

/// Texture and sampler pools may have changed (see above).
inline void Nx2BumpPoolGeneration() {
    if (Nx2QuickBind() != 0) {
        g_nx2_pool_generation.fetch_add(1, std::memory_order_relaxed);
    }
}

struct Nx2QuickBindStats {
    std::atomic<u64> draws{};      ///< Draws that could use a quick bind (switch on, no split)
    std::atomic<u64> hits{};       ///< Of draws: reused the previous views and samplers
    std::atomic<u64> mismatches{}; ///< verify: hits whose full resolution differed
    std::atomic<u64> stale{};      ///< verify: hits with an image the full path would prepare
    std::atomic<u64> vertex_draws{};   ///< EDEN_NX2_FAST_BUFFERS: draws
    std::atomic<u64> vertex_skipped{}; ///< Vertex buffer synchronizations not made (unread)
    std::atomic<u64> vertex_pending{}; ///< Of those: the stream had CPU writes to upload
    std::atomic<u64> window_start{};
};
inline Nx2QuickBindStats g_nx2_quick_bind{};

/// Logs and resets the counts every 10 s (nx2_strato.cpp).
void Nx2ReportQuickBind();

} // namespace Tegra
