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

    /// A successful result with one text block.
    static ToolResult Text(std::string text);
    /// A result with one text block and is_error set.
    static ToolResult Error(std::string text);
    /// Sets structuredContent and mirrors it as a text block for clients that ignore it.
    static ToolResult Structured(json value);

    /// Appends a text content block. Returns *this for chaining.
    ToolResult& AddText(std::string text);
    /// Appends an image content block with the bytes base64-encoded. Returns *this for chaining.
    ToolResult& AddImage(std::span<const u8> bytes, std::string mime_type);

    /// The MCP CallToolResult object (content, structuredContent, isError).
    [[nodiscard]] json ToJson() const;
};

/// Implements a tool. Runs on an HTTP worker thread; arguments have already been validated
/// against the tool's input_schema. May throw; the server turns exceptions into errors.
using ToolHandler = std::function<ToolResult(ToolContext& context, const json& arguments)>;

/// A tool definition plus its handler, as registered in the ToolRegistry.
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

    /// The tool's entry for tools/list (name, schemas, annotations, title).
    [[nodiscard]] json DefinitionJson() const;
};

/// Standard base64 with padding.
std::string Base64Encode(std::span<const u8> bytes);

} // namespace Mcp
