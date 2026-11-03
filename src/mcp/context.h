// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

    [[nodiscard]] bool GameRunning() const;
};

} // namespace Mcp
