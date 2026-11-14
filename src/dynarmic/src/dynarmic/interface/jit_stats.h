// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Dynarmic {

/// Running totals over every JIT instance (read by the frontend's frame spike recorder).
struct JitStats {
    std::atomic<std::uint64_t> blocks_compiled{};
    std::atomic<std::uint64_t> compile_ns{}; ///< Translate, optimize and emit (x64 A64 only)
    std::atomic<std::uint64_t> fastmem_recompiles{}; ///< Blocks invalidated by a fastmem fault
};

inline JitStats g_jit_stats;

} // namespace Dynarmic
