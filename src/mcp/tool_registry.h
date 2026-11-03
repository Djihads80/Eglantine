// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mcp/tool.h"

namespace Mcp {

class ToolRegistry {
public:
    /// Adds a tool. Names must be unique snake_case; asserts otherwise.
    void Add(Tool tool);

    [[nodiscard]] const Tool* Find(std::string_view name) const;
    [[nodiscard]] json ListJson() const;
    [[nodiscard]] size_t Size() const {
        return tools.size();
    }

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
