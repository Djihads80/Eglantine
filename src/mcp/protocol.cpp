// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <random>

#include <fmt/format.h>

#include "common/logging.h"
#include "mcp/control.h"
#include "mcp/activity.h"
#include "mcp/protocol.h"
#include "mcp/tool_registry.h"

namespace Mcp {

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

/// Whether version is one of kSupportedProtocolVersions.
bool IsSupportedVersion(const std::string& version) {
    return std::any_of(std::begin(kSupportedProtocolVersions), std::end(kSupportedProtocolVersions),
                       [&](const char* v) { return version == v; });
}

HttpResponse JsonResponse(int status, const json& body) {
    return HttpResponse{status, "application/json", body.dump(), {}};
}

/// A response with only a status code and no body.
HttpResponse Empty(int status) {
    return HttpResponse{status, "", "", {}};
}

} // namespace

json JsonRpcError(const json& id, int code, std::string message) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", code}, {"message", std::move(message)}}}};
}

/// Value of the named header (lowercase), or nullopt when absent.
std::optional<std::string> HttpRequest::Header(const std::string& lowercase_name) const {
    const auto it = headers.find(lowercase_name);
    if (it == headers.end()) {
        return std::nullopt;
    }
    return it->second;
}

Protocol::Protocol(const ToolRegistry& registry_, ProtocolOptions options_,
                   ContextFactory context_factory_)
    : registry{registry_}, options{std::move(options_)},
      context_factory{std::move(context_factory_)} {}

/// True if `origin` is a loopback http(s) origin.
bool Protocol::IsAllowedOrigin(const std::string& origin) {
    // Browsers send Origin on cross-site requests; only loopback pages may talk to the server
    // (defends against DNS rebinding). Non-browser clients usually send no Origin at all.
    for (const char* allowed : {"http://localhost", "https://localhost", "http://127.0.0.1",
                                "https://127.0.0.1", "http://[::1]", "https://[::1]"}) {
        const std::string_view prefix{allowed};
        if (origin.starts_with(prefix)) {
            const std::string_view rest = std::string_view{origin}.substr(prefix.size());
            if (rest.empty() || rest[0] == ':' || rest[0] == '/') {
                return true;
            }
        }
    }
    return false;
}

/// True if the Host header is empty or names a loopback address, with an optional port.
bool Protocol::IsAllowedHost(const std::string& host) {
    // A DNS-rebinding page is same-origin with the server (so browsers send no Origin), but its
    // requests still carry the attacker's host name. Browsers always send Host; other clients
    // that omit it are not a rebinding risk.
    if (host.empty()) {
        return true;
    }
    for (const char* allowed : {"localhost", "127.0.0.1", "[::1]"}) {
        const std::string_view name{allowed};
        if (host.starts_with(name)) {
            const std::string_view rest = std::string_view{host}.substr(name.size());
            if (rest.empty() || rest[0] == ':') {
                return true;
            }
        }
    }
    return false;
}

size_t Protocol::SessionCount() const {
    std::scoped_lock lock{sessions_mutex};
    return sessions.size();
}

bool Protocol::HasSession(const std::string& id) const {
    std::scoped_lock lock{sessions_mutex};
    return sessions.contains(id);
}

/// Checks path, host, origin and token, then serves POST or deletes the session on DELETE.
HttpResponse Protocol::Handle(const HttpRequest& request) {
    if (request.path != "/mcp") {
        return Empty(404);
    }
    if (const auto host = request.Header("host"); host && !IsAllowedHost(*host)) {
        LOG_WARNING(Debug_MCP, "Rejected request for host {}", *host);
        return Empty(403);
    }
    if (const auto origin = request.Header("origin"); origin && !IsAllowedOrigin(*origin)) {
        LOG_WARNING(Debug_MCP, "Rejected request from origin {}", *origin);
        return Empty(403);
    }
    if (!options.bearer_token.empty()) {
        const auto auth = request.Header("authorization");
        if (!auth || *auth != "Bearer " + options.bearer_token) {
            return Empty(401);
        }
    }
    if (request.method == "POST") {
        return HandlePost(request);
    }
    if (request.method == "DELETE") {
        const auto id = request.Header("mcp-session-id");
        if (!id) {
            return Empty(400);
        }
        bool erased = false;
        {
            std::scoped_lock lock{sessions_mutex};
            if (const auto it = sessions.find(*id); it != sessions.end()) {
                it->second.stop.request_stop();
                sessions.erase(it);
                erased = true;
            }
        }
        if (erased) {
            ReleaseLease(*id);
        }
        return Empty(erased ? 204 : 404);
    }
    // No standalone server-to-client SSE stream yet.
    HttpResponse response = Empty(405);
    response.headers.emplace_back("Allow", "POST, DELETE");
    return response;
}

/// Validates a JSON-RPC message and its session, then dispatches requests that expect a reply.
HttpResponse Protocol::HandlePost(const HttpRequest& request) {
    if (request.body.size() > options.max_body_bytes) {
        return Empty(413);
    }
    if (const auto accept = request.Header("accept");
        accept && accept->find("application/json") == std::string::npos &&
        accept->find("*/*") == std::string::npos) {
        return Empty(406);
    }

    const json message = json::parse(request.body, nullptr, false);
    if (message.is_discarded()) {
        return JsonResponse(400, JsonRpcError(nullptr, kParseError, "Parse error"));
    }
    if (message.is_array()) {
        return JsonResponse(400, JsonRpcError(nullptr, kInvalidRequest,
                                              "JSON-RPC batching is not supported"));
    }
    // Type-checked first: value() throws when the member exists with another type.
    if (!message.is_object() || !message.contains("jsonrpc") || !message["jsonrpc"].is_string() ||
        message["jsonrpc"].get<std::string>() != "2.0") {
        return JsonResponse(400, JsonRpcError(nullptr, kInvalidRequest, "Invalid Request"));
    }

    const bool has_method = message.contains("method") && message["method"].is_string();
    const bool has_id = message.contains("id") && !message["id"].is_null();
    const std::string method = has_method ? message["method"].get<std::string>() : "";
    const json id = has_id ? message["id"] : json(nullptr);

    if (method != "initialize") {
        const auto session_id = request.Header("mcp-session-id");
        if (!session_id) {
            return JsonResponse(400, JsonRpcError(id, kInvalidRequest,
                                                  "Missing Mcp-Session-Id; call initialize first"));
        }
        std::scoped_lock lock{sessions_mutex};
        const auto it = sessions.find(*session_id);
        if (it == sessions.end()) {
            // Unknown or expired (e.g. the emulator restarted): the client re-initializes.
            return Empty(404);
        }
        if (const auto version = request.Header("mcp-protocol-version");
            version && !IsSupportedVersion(*version)) {
            return JsonResponse(400, JsonRpcError(id, kInvalidRequest,
                                                  "Unsupported MCP-Protocol-Version " + *version));
        }
        it->second.last_used = std::chrono::steady_clock::now();
    }

    if (!has_method || !has_id) {
        // Notifications (initialized, cancelled, ...) and client responses need no reply.
        return Empty(202);
    }

    std::string new_session_id;
    int status = 200;
    const auto reply = Dispatch(message, request, new_session_id, status);
    HttpResponse response = JsonResponse(status, reply.value_or(json::object()));
    if (!new_session_id.empty()) {
        response.headers.emplace_back("Mcp-Session-Id", new_session_id);
    }
    return response;
}

/// Routes a JSON-RPC request to its method handler and returns the reply.
std::optional<json> Protocol::Dispatch(const json& message, const HttpRequest& request,
                                       std::string& new_session_id, [[maybe_unused]] int& status) {
    const std::string method = message["method"].get<std::string>();
    const json& id = message["id"];
    const json params = message.value("params", json::object());

    const auto ok = [&](json result) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    };

    if (method == "initialize") {
        return ok(HandleInitialize(params, new_session_id));
    }
    if (method == "ping") {
        return ok(json::object());
    }
    if (method == "tools/list") {
        return ok(json{{"tools", registry.ListJson()}});
    }
    if (method == "tools/call") {
        return HandleToolsCall(id, params, request.Header("mcp-session-id").value_or(""));
    }
    if (method == "logging/setLevel") {
        return ok(json::object()); // the SSE stream sends warning+ regardless
    }
    if (method == "resources/list") {
        json list = json::array();
        for (const auto& r : options.resources) {
            list.push_back({{"uri", r.uri},
                            {"name", r.name},
                            {"description", r.description},
                            {"mimeType", r.mime_type}});
        }
        return ok(json{{"resources", std::move(list)}});
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        for (const auto& r : options.resources) {
            if (r.uri == uri) {
                ToolContext context = context_factory();
                std::string text;
                try {
                    text = r.read(context);
                } catch (const std::exception& e) {
                    return JsonRpcError(id, kInternalError, e.what());
                }
                return ok(json{{"contents", json::array({{{"uri", uri},
                                                          {"mimeType", r.mime_type},
                                                          {"text", std::move(text)}}})}});
            }
        }
        return JsonRpcError(id, kInvalidParams, "Unknown resource: " + uri);
    }
    if (method == "prompts/list") {
        json list = json::array();
        for (const auto& p : options.prompts) {
            list.push_back({{"name", p.name}, {"description", p.description}, {"arguments", p.arguments}});
        }
        return ok(json{{"prompts", std::move(list)}});
    }
    if (method == "prompts/get") {
        const std::string name = params.value("name", "");
        for (const auto& p : options.prompts) {
            if (p.name == name) {
                const json args = params.value("arguments", json::object());
                std::string text;
                try {
                    text = p.render(args);
                } catch (const std::exception& e) {
                    return JsonRpcError(id, kInternalError,
                                        fmt::format("Prompt {} failed: {}", name, e.what()));
                }
                return ok(json{{"description", p.description},
                               {"messages", json::array({{{"role", "user"},
                                                          {"content", {{"type", "text"},
                                                                       {"text", text}}}}})}});
            }
        }
        return JsonRpcError(id, kInvalidParams, "Unknown prompt: " + name);
    }
    return JsonRpcError(id, kMethodNotFound, "Method not found: " + method);
}

/// Negotiates the protocol version, creates a session and returns the server capabilities.
json Protocol::HandleInitialize(const json& params, std::string& new_session_id) {
    std::string version = kSupportedProtocolVersions[0];
    if (params.is_object() && params.contains("protocolVersion") &&
        params["protocolVersion"].is_string() &&
        IsSupportedVersion(params["protocolVersion"].get<std::string>())) {
        version = params["protocolVersion"].get<std::string>();
    }
    new_session_id = CreateSession(version);
    if (params.is_object() && params.contains("clientInfo")) {
        LOG_INFO(Debug_MCP, "Client connected: {} (protocol {})", params["clientInfo"].dump(),
                 version);
    }
    json capabilities{{"tools", {{"listChanged", false}}}, {"logging", json::object()}};
    if (!options.resources.empty()) {
        capabilities["resources"] = {{"listChanged", false}, {"subscribe", false}};
    }
    if (!options.prompts.empty()) {
        capabilities["prompts"] = {{"listChanged", false}};
    }
    json result{
        {"protocolVersion", version},
        {"capabilities", std::move(capabilities)},
        {"serverInfo",
         {{"name", "eglantine"}, {"title", "Eglantine emulator"}, {"version", options.server_version}}},
    };
    if (!options.instructions.empty()) {
        result["instructions"] = options.instructions;
    }
    return result;
}

/// Validates and runs a tool call, refusing it when writes, a game or the lease are missing.
json Protocol::HandleToolsCall(const json& id, const json& params,
                                const std::string& session_id) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return JsonRpcError(id, kInvalidParams, "tools/call requires a string 'name'");
    }
    const std::string name = params["name"].get<std::string>();
    const Tool* tool = registry.Find(name);
    if (tool == nullptr) {
        return JsonRpcError(id, kInvalidParams, "Unknown tool: " + name);
    }
    const json arguments = params.value("arguments", json::object());
    Activity::Get().Update([&](ActivityState& s) {
        s.last_tool = name;
        s.last_tool_time = std::chrono::steady_clock::now();
        ++s.tool_calls;
    });
    if (auto error = ValidateArguments(tool->input_schema, arguments)) {
        return JsonRpcError(id, kInvalidParams, "Invalid arguments: " + *error);
    }

    ToolContext context = context_factory ? context_factory() : ToolContext{};
    context.session_id = session_id;
    // The session's stop token: ending the session stops input this call is still sending.
    bool session_live = session_id.empty();
    {
        std::scoped_lock lock{sessions_mutex};
        if (const auto it = sessions.find(session_id); it != sessions.end()) {
            context.stop = it->second.stop.get_token();
            session_live = true;
        }
    }
    // Cheap refusals come first, so a refused call never takes the control lease.
    const auto refusal = [&](const ToolContext& c) -> std::optional<ToolResult> {
        if (tool->requires_write && !c.writes_allowed) {
            return ToolResult::Error(
                "This tool changes emulator state and MCP writes are disabled. Ask the user to "
                "enable Tools > MCP Server > Allow Writes, or relaunch with --mcp-allow-writes.");
        }
        if (tool->requires_game && !c.GameRunning()) {
            return ToolResult::Error("No application is running. Boot a game first (boot_game).");
        }
        return std::nullopt;
    };
    const auto lease_refusal = [&]() -> std::optional<ToolResult> {
        // A call whose session ended meanwhile must not take the freed lease.
        if (tool->requires_control && (!session_live || context.stop.stop_requested())) {
            return ToolResult::Error("This MCP session has ended; initialize a new one.");
        }
        ControlLease::Holder holder;
        if (!tool->requires_control || !context.control ||
            context.control->Touch(session_id, holder)) {
            return std::nullopt;
        }
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                              holder.expires - ControlLease::Clock::now())
                              .count();
        return ToolResult::Error(fmt::format(
            "Another agent controls the emulator ({}session {}..., lease expires in {} s unless "
            "it keeps acting). Read-only tools still work. Call control_status to check, or "
            "acquire_control once it is free.",
            holder.label.empty() ? "" : holder.label + ", ", holder.session_id.substr(0, 8),
            left));
    };
    ToolResult result;
    if (auto refused = refusal(context)) {
        result = std::move(*refused);
    } else if (auto refused_lease = lease_refusal()) {
        result = std::move(*refused_lease);
    } else {
        try {
            if (tool->annotations.read_only) {
                result = tool->handler(context, arguments);
            } else {
                std::scoped_lock lock{mutating_tools_mutex};
                // Writes or the lease may have changed while this call waited for the lock.
                if (tool->requires_write && context_factory) {
                    context.writes_allowed = context_factory().writes_allowed;
                }
                if (auto late = refusal(context)) {
                    result = std::move(*late);
                } else if (auto late_lease = lease_refusal()) {
                    result = std::move(*late_lease);
                } else {
                    result = tool->handler(context, arguments);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Debug_MCP, "Tool {} threw: {}", name, e.what());
            return JsonRpcError(id, kInternalError, fmt::format("Tool {} failed: {}", name, e.what()));
        }
    }
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result.ToJson()}};
}

/// Adds a session with a random id, evicting the least recently used one when full.
std::string Protocol::CreateSession(std::string protocol_version) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::string id = fmt::format("{:016x}{:016x}", rng(), rng());
    std::string evicted;
    {
        std::scoped_lock lock{sessions_mutex};
        if (sessions.size() >= options.max_sessions) {
            const auto oldest = std::min_element(
                sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
                    return a.second.last_used < b.second.last_used;
                });
            evicted = oldest->first;
            oldest->second.stop.request_stop();
            sessions.erase(oldest);
        }
        sessions.emplace(id,
                         Session{std::move(protocol_version), std::chrono::steady_clock::now()});
    }
    if (!evicted.empty()) {
        ReleaseLease(evicted);
    }
    return id;
}

void Protocol::ReleaseLease(const std::string& session_id) {
    // First stop work the session left running (a background route could otherwise keep
    // sending input after another agent takes control), then free its lease.
    if (options.on_session_end) {
        options.on_session_end(session_id);
    }
    if (!context_factory) {
        return;
    }
    if (ControlLease* lease = context_factory().control) {
        lease->Release(session_id);
    }
}

} // namespace Mcp
