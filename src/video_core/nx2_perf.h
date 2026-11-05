// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

// EXPERIMENT (not for nx2): GPU back-thread optimizations for Ocarina of Time, all on by default.
// EDEN_NX2_PERF_OPT=0 turns every one off for the A/B. EDEN_NX2_PERF_VERIFY=1 computes each
// optimized answer the old way too, logs any difference and uses the old answer.
namespace VideoCommon::Nx2Perf {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_OPT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

} // namespace VideoCommon::Nx2Perf
