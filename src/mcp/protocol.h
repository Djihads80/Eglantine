// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
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

/// A parsed HTTP request as handed to Protocol by the transport.
struct HttpRequest {
    std::string method; ///< "GET", "POST", "DELETE", ...
    std::string path;
    std::map<std::string, std::string> headers; ///< Keys lowercased.
    std::string body;

    /// Returns the value of a header, or nullopt if absent. The name must be lowercase.
    [[nodiscard]] std::optional<std::string> Header(const std::string& lowercase_name) const;
};

/// An HTTP response for the transport to send. An empty content_type means no body.
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

/// Configuration for a Protocol instance, fixed at construction.
struct ProtocolOptions {
    std::string server_version = "dev";
    std::string instructions;
    std::vector<Resource> resources;
    std::vector<Prompt> prompts;
    /// When non-empty, requests must carry "Authorization: Bearer <token>".
    std::string bearer_token;
    size_t max_body_bytes = 1 << 20;
    size_t max_sessions = 16;
    /// Called when a session ends (DELETE or eviction), before its control lease is freed, to
    /// stop background work it started.
    std::function<void(const std::string& session_id)> on_session_end;
};

/// Transport-independent MCP server core: Streamable HTTP request rules, sessions, version
/// negotiation and JSON-RPC dispatch to the tool registry. Thread-safe.
class Protocol {
public:
    /// Builds a fresh ToolContext for each tool call or resource read.
    using ContextFactory = std::function<ToolContext()>;

    /// The registry must outlive the Protocol.
    Protocol(const ToolRegistry& registry, ProtocolOptions options, ContextFactory context_factory);

    /**
     * Handles one request to /mcp: checks Host, Origin and the bearer token, then serves POST
     * (JSON-RPC) and DELETE (end session). Other methods get 405. May be called from any thread;
     * blocks while a tool runs, and non-read-only tools are serialized.
     *
     * @param request The parsed HTTP request.
     *
     * @return The response to send. Failures are reported as HTTP status codes or JSON-RPC errors.
     */
    HttpResponse Handle(const HttpRequest& request);

    /// Number of live sessions.
    [[nodiscard]] size_t SessionCount() const;

    /// Whether id names a live session (for the SSE stream on GET /mcp).
    [[nodiscard]] bool HasSession(const std::string& id) const;

    /// Exposed for tests.
    [[nodiscard]] static bool IsAllowedOrigin(const std::string& origin);
    /// Host header check against DNS rebinding (loopback names only).
    [[nodiscard]] static bool IsAllowedHost(const std::string& host);

private:
    /// State kept per Mcp-Session-Id. Guarded by sessions_mutex.
    struct Session {
        std::string protocol_version;
        std::chrono::steady_clock::time_point last_used;
        /// Requested when the session ends: input its calls are still sending stops.
        std::stop_source stop;
    };

    /// Validates a POST body and session, then dispatches requests. Notifications get 202.
    HttpResponse HandlePost(const HttpRequest& request);
    /// Routes one JSON-RPC request by method. Sets new_session_id on initialize.
    std::optional<json> Dispatch(const json& message, const HttpRequest& request,
                                 std::string& new_session_id, int& http_status);
    /// Negotiates the protocol version, creates a session and returns server capabilities.
    json HandleInitialize(const json& params, std::string& new_session_id);
    /**
     * Runs a tool after validating arguments and checking writes, game state and the control
     * lease. Tools that are not read-only run under mutating_tools_mutex and are re-checked
     * once the lock is held.
     *
     * @return A JSON-RPC response. Refusals are tool results with isError set; unknown tools,
     *         bad arguments and exceptions thrown by the handler are JSON-RPC errors.
     */
    json HandleToolsCall(const json& id, const json& params, const std::string& session_id);

    /// Creates a session with a random id and returns the id. Evicts the least recently used
    /// session when max_sessions is reached.
    std::string CreateSession(std::string protocol_version);
    /// Frees the control lease if the given (ended) session holds it, so other agents are not
    /// locked out until it expires.
    void ReleaseLease(const std::string& session_id);

    const ToolRegistry& registry;
    ProtocolOptions options;
    ContextFactory context_factory;

    mutable std::mutex sessions_mutex;
    std::map<std::string, Session> sessions;

    /// Serializes tools that are not read-only (boot, pause, writes...).
    std::mutex mutating_tools_mutex;
};

/// Builds a JSON-RPC 2.0 error response object.
json JsonRpcError(const json& id, int code, std::string message);

} // namespace Mcp
