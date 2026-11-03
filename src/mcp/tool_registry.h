// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mcp/tool.h"

namespace Mcp {

/// The set of tools the server offers, keyed and ordered by name. Filled once at startup and
/// read-only afterwards, so concurrent lookups need no lock.
class ToolRegistry {
public:
    /// Adds a tool. Names must be unique snake_case; asserts otherwise.
    void Add(Tool tool);

    /// Returns the tool with this name, or nullptr. The pointer lives as long as the registry.
    [[nodiscard]] const Tool* Find(std::string_view name) const;
    /// The tools/list array: every tool's DefinitionJson, sorted by name.
    [[nodiscard]] json ListJson() const;
    [[nodiscard]] size_t Size() const {
        return tools.size();
    }

    /// Calls fn(const Tool&) for every tool in name order.
    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (const auto& [name, tool] : tools) {
            fn(tool);
        }
    }

private:
    std::map<std::string, Tool, std::less<>> tools;
};

/// Validates arguments against the subset of JSON Schema the tools use: type, properties,
/// required, additionalProperties:false, enum, minimum/maximum, minLength/maxLength,
/// minItems/maxItems and items. Returns an error message, or nullopt when valid.
std::optional<std::string> ValidateArguments(const json& schema, const json& value,
                                             const std::string& path = "arguments");

/// True for names made of lowercase letters, digits and underscores, starting with a letter.
bool IsValidToolName(std::string_view name);

} // namespace Mcp
