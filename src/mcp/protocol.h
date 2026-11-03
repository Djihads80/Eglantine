// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mcp/context.h"
#include "mcp/tool.h"

namespace Mcp {

class ToolRegistry;

/// Newest first. The first entry is offered when a client asks for an unknown version.
inline constexpr const char* kSupportedProtocolVersions[] = {"2025-11-25", "2025-06-18",
                                                              "2025-03-26"};

struct HttpRequest {
    std::string method; ///< "GET", "POST", "DELETE", ...
    std::string path;
    std::map<std::string, std::string> headers; ///< Keys lowercased.
    std::string body;

    [[nodiscard]] std::optional<std::string> Header(const std::string& lowercase_name) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// A readable MCP resource (resources/list, resources/read).
struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type = "application/json";
    std::function<std::string(ToolContext&)> read;
};

/// An MCP prompt template (prompts/list, prompts/get).
struct Prompt {
    std::string name;
    std::string description;
    json arguments = json::array(); ///< [{name, description, required}]
    std::function<std::string(const json& arguments)> render;
};

struct ProtocolOptions {
    std::string server_version = "dev";
    std::string instructions;
    std::vector<Resource> resources;
    std::vector<Prompt> prompts;
    /// When non-empty, requests must carry "Authorization: Bearer <token>".
    std::string bearer_token;
    size_t max_body_bytes = 1 << 20;
    size_t max_sessions = 16;
};

/// Transport-independent MCP server core: Streamable HTTP request rules, sessions, version
/// negotiation and JSON-RPC dispatch to the tool registry. Thread-safe.
class Protocol {
public:
    using ContextFactory = std::function<ToolContext()>;

    Protocol(const ToolRegistry& registry, ProtocolOptions options, ContextFactory context_factory);

    HttpResponse Handle(const HttpRequest& request);

    [[nodiscard]] size_t SessionCount() const;

    /// Whether id names a live session (for the SSE stream on GET /mcp).
    [[nodiscard]] bool HasSession(const std::string& id) const;

    /// Exposed for tests.
    [[nodiscard]] static bool IsAllowedOrigin(const std::string& origin);
    /// Host header check against DNS rebinding (loopback names only).
    [[nodiscard]] static bool IsAllowedHost(const std::string& host);

private:
    struct Session {
        std::string protocol_version;
        std::chrono::steady_clock::time_point last_used;
    };

    HttpResponse HandlePost(const HttpRequest& request);
    std::optional<json> Dispatch(const json& message, const HttpRequest& request,
                                 std::string& new_session_id, int& http_status);
    json HandleInitialize(const json& params, std::string& new_session_id);
    json HandleToolsCall(const json& id, const json& params, const std::string& session_id);

    std::string CreateSession(std::string protocol_version);

    const ToolRegistry& registry;
    ProtocolOptions options;
    ContextFactory context_factory;

    mutable std::mutex sessions_mutex;
    std::map<std::string, Session> sessions;

    /// Serializes tools that are not read-only (boot, pause, writes...).
    std::mutex mutating_tools_mutex;
};

json JsonRpcError(const json& id, int code, std::string message);

} // namespace Mcp
