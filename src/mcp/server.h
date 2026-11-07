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

/// Listen address and access settings for the MCP server.
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
    /// Registers all tools and builds the protocol, resources and prompts. Does not listen yet.
    /// system and host must outlive the Server.
    Server(Core::System& system, Host& host, ServerConfig config);
    /// Stops the server if it is running.
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds and starts serving. Returns an empty string on success, else an error message.
    /// Also installs the log sink and trace hooks, reopens the timeline, starts the HTTP thread
    /// and writes the discovery file. Does nothing if already running.
    std::string Start();
    /// Wakes all blocked requests, removes the log sink, hooks and discovery file, then joins the
    /// HTTP thread. Blocks until in-flight handlers return. Safe to call when not running.
    void Stop();

    [[nodiscard]] bool IsRunning() const {
        return running.load();
    }
    [[nodiscard]] const ServerConfig& Config() const {
        return config;
    }
    /// The MCP endpoint URL, e.g. "http://127.0.0.1:6544/mcp".
    [[nodiscard]] std::string Url() const;

    /// Enables or disables state-changing tools for agents. Takes effect on the next call.
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
    /// Snapshot of the server state for the status bar. Takes the session and lease locks.
    [[nodiscard]] Summary GetSummary();

    /// Runs a tool for the frontend's own UI (the user, not an agent: no lease or write checks).
    /// Must not be used for tools that marshal to the UI thread when called from it.
    ToolResult CallTool(const std::string& name, const json& arguments);

private:
    /// Builds a ToolContext with the current writes flag and no session id.
    ToolContext MakeContext();

    Core::System& system;
    Host& host;
    ServerConfig config;
    std::atomic<bool> writes_allowed;

    Timeline& timeline; ///< Process-lifetime (see ProcessTimeline in server.cpp)
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
