// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <stop_token>
#include <string>

namespace Core {
class System;
}

namespace Mcp {

class ControlLease;
class Host;
class Timeline;

/// Everything a tool handler may use. Pointers can be null in unit tests.
struct ToolContext {
    Core::System* system = nullptr;
    Host* host = nullptr;
    Timeline* timeline = nullptr;
    bool writes_allowed = false;
    ControlLease* control = nullptr;
    std::string session_id; ///< MCP session making the call.
    /// The server's live Allow Writes flag, for work that outlasts the call (background routes).
    const std::atomic<bool>* live_writes = nullptr;
    /// Set for background work (routes, A/B tests): input steps end early when it is requested.
    std::stop_token stop;

    /// True when a system is set, powered on and not shutting down.
    [[nodiscard]] bool GameRunning() const;
};

} // namespace Mcp
