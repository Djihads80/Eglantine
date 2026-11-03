// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "mcp/control.h"
#include "mcp/protocol.h"
#include "mcp/timeline.h"
#include "mcp/activity.h"
#include "mcp/tool_registry.h"

namespace Core {
class System;
}

namespace httplib {
class Server;
}

namespace Mcp {

class Host;

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    u16 port = 6544;
    bool allow_writes = false;
    std::string bearer_token;
};

/// The built-in MCP server: Streamable HTTP on loopback, owned by the frontend for the lifetime
/// of the application (it outlives individual game boots).
class Server {
public:
    Server(Core::System& system, Host& host, ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds and starts serving. Returns an empty string on success, else an error message.
    std::string Start();
    void Stop();

    [[nodiscard]] bool IsRunning() const {
        return running.load();
    }
    [[nodiscard]] const ServerConfig& Config() const {
        return config;
    }
    [[nodiscard]] std::string Url() const;

    void SetWritesAllowed(bool allowed) {
        writes_allowed.store(allowed);
    }

    Timeline& GetTimeline() {
        return timeline;
    }
    ToolRegistry& Registry() {
        return registry;
    }
    ControlLease& Control() {
        return control;
    }

    /// The running server's timeline, for producers outside the MCP library (log sink, hooks).
    /// Null when no server runs.
    static Timeline* ActiveTimeline();

    /// For the frontend's status bar: what the server and its agents are doing.
    struct Summary {
        std::string url;
        size_t sessions = 0;
        std::string controller; ///< Control lease holder (label or session prefix), or empty
        bool writes = false;
        ActivityState activity;
    };
    [[nodiscard]] Summary GetSummary();

    /// Runs a tool for the frontend's own UI (the user, not an agent: no lease or write checks).
    /// Must not be used for tools that marshal to the UI thread when called from it.
    ToolResult CallTool(const std::string& name, const json& arguments);

private:
    ToolContext MakeContext();

    Core::System& system;
    Host& host;
    ServerConfig config;
    std::atomic<bool> writes_allowed;

    Timeline timeline;
    ControlLease control;
    ToolRegistry registry;
    std::unique_ptr<Protocol> protocol;

    std::unique_ptr<httplib::Server> http;
    std::thread http_thread;
    std::atomic<bool> running{false};
};

/// Registers every built-in tool.
void RegisterAllTools(ToolRegistry& registry);

} // namespace Mcp
