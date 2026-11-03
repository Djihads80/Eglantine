// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mcp/tool.h"

namespace Mcp {

ToolResult ToolResult::Text(std::string text) {
    ToolResult result;
    result.AddText(std::move(text));
    return result;
}

ToolResult ToolResult::Error(std::string text) {
    ToolResult result = Text(std::move(text));
    result.is_error = true;
    return result;
}

ToolResult ToolResult::Structured(json value) {
    ToolResult result;
    result.AddText(value.dump(2));
    result.structured = std::move(value);
    return result;
}

ToolResult& ToolResult::AddText(std::string text) {
    content.push_back(json{{"type", "text"}, {"text", std::move(text)}});
    return *this;
}

ToolResult& ToolResult::AddImage(std::span<const u8> bytes, std::string mime_type) {
    content.push_back(
        json{{"type", "image"}, {"data", Base64Encode(bytes)}, {"mimeType", std::move(mime_type)}});
    return *this;
}

/// The MCP tools/call result object: content, structuredContent and isError.
json ToolResult::ToJson() const {
    json out{{"content", content}};
    if (structured) {
        out["structuredContent"] = *structured;
    }
    if (is_error) {
        out["isError"] = true;
    }
    return out;
}

/// The tool's entry for tools/list: name, schemas, title and annotations.
json Tool::DefinitionJson() const {
    json out{
        {"name", name},
        {"description", description},
        {"inputSchema", input_schema},
        {"annotations",
         {{"readOnlyHint", annotations.read_only},
          {"destructiveHint", annotations.destructive},
          {"idempotentHint", annotations.idempotent},
          {"openWorldHint", annotations.open_world}}},
    };
    if (!title.empty()) {
        out["title"] = title;
        out["annotations"]["title"] = title;
    }
    if (output_schema) {
        out["outputSchema"] = *output_schema;
    }
    return out;
}

/// Standard base64 encoding with padding.
std::string Base64Encode(std::span<const u8> bytes) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32{bytes[i]} << 16) | (u32{bytes[i + 1]} << 8) | bytes[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < bytes.size()) {
        u32 v = u32{bytes[i]} << 16;
        if (i + 1 < bytes.size()) {
            v |= u32{bytes[i + 1]} << 8;
        }
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace Mcp
