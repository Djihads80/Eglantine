// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <utility>

namespace VideoCommon {

inline bool NativeMmeQueryFlushEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MME_QUERY_FLUSH");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Force only this fence's callback to retire after query writebacks, including callbacks
// created by CommitAsyncFlushes itself. A plain SyncOperation before SignalReference would
// precede those late callbacks. The caller owns the backend and must submit even without
// new draws; it may wait only after this function returns.
template <typename FenceManager, typename Complete, typename Submit>
void SubmitQueryReadFence(FenceManager& fences, Complete&& complete, Submit&& submit) {
    fences.SignalFence(std::forward<Complete>(complete), true);
    std::forward<Submit>(submit)();
}

} // namespace VideoCommon
