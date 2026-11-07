// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the MCP server is doing right now (last tool call, recording, route progress), for the
// frontend's status bar and the dashboard. Written by tools, read by the UI; thread-safe.

#include <chrono>
#include <mutex>
#include <string>

#include "common/common_types.h"

namespace Mcp {

/// Plain data copied out by Activity::Snapshot.
struct ActivityState {
    std::string last_tool;
    std::chrono::steady_clock::time_point last_tool_time{};
    u64 tool_calls = 0;

    bool recording = false;
    std::string record_name;
    u64 record_frames = 0;
    u64 record_checkpoints = 0;

    std::string route;       ///< Route running or last run
    std::string route_state; ///< "running", "done", "failed", "stopped" or empty
    size_t route_step = 0;   ///< Steps completed
    size_t route_total = 0;
    std::string route_detail; ///< e.g. "waiting for checkpoint 3"
    std::chrono::steady_clock::time_point route_changed{}; ///< Last state change (UIs fade "done")
};

/// Process-wide holder of the current ActivityState, guarded by a mutex.
class Activity {
public:
    /// The single instance.
    static Activity& Get() {
        static Activity instance;
        return instance;
    }

    /// Calls fn(ActivityState&) with the lock held. fn must be short and must not call back
    /// into Activity.
    template <typename Fn>
    void Update(Fn&& fn) {
        std::scoped_lock lock{mutex};
        fn(state);
    }

    /// A copy of the current state.
    [[nodiscard]] ActivityState Snapshot() {
        std::scoped_lock lock{mutex};
        return state;
    }

private:
    std::mutex mutex;
    ActivityState state;
};

} // namespace Mcp
