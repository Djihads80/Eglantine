// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "common/common_types.h"

namespace Mcp {

using json = nlohmann::json;

struct ToolContext;

/// MCP tool annotations (hints for clients; not enforced by the protocol).
struct ToolAnnotations {
    bool read_only = false;
    bool destructive = false;
    bool idempotent = false;
    bool open_world = false;
};

/// Result of a tools/call. Domain failures are returned as results with is_error set, so the
/// model sees them; protocol failures are JSON-RPC errors raised by the server instead.
struct ToolResult {
    json content = json::array();
    std::optional<json> structured;
    bool is_error = false;

    static ToolResult Text(std::string text);
    static ToolResult Error(std::string text);
    /// Sets structuredContent and mirrors it as a text block for clients that ignore it.
    static ToolResult Structured(json value);

    ToolResult& AddText(std::string text);
    ToolResult& AddImage(std::span<const u8> bytes, std::string mime_type);

    [[nodiscard]] json ToJson() const;
};

using ToolHandler = std::function<ToolResult(ToolContext& context, const json& arguments)>;

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    json input_schema = json{{"type", "object"}, {"properties", json::object()}};
    std::optional<json> output_schema;
    ToolAnnotations annotations;
    /// Rejected with a tool error when no application is running.
    bool requires_game = false;
    /// Rejected with a tool error unless MCP writes are enabled.
    bool requires_write = false;
    /// Needs the control lease (one agent at a time); granted automatically when free.
    bool requires_control = false;
    ToolHandler handler;

    [[nodiscard]] json DefinitionJson() const;
};

std::string Base64Encode(std::span<const u8> bytes);

} // namespace Mcp
