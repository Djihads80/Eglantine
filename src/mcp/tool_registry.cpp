// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

#include "common/assert.h"
#include "mcp/tool_registry.h"

namespace Mcp {

/// True for 1 to 64 characters of [a-z0-9_] starting with a lowercase letter.
bool IsValidToolName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/// Registers a tool, asserting that it is well formed and its name is unique.
void ToolRegistry::Add(Tool tool) {
    ASSERT_MSG(IsValidToolName(tool.name), "invalid MCP tool name '{}'", tool.name);
    ASSERT_MSG(!tool.description.empty(), "MCP tool '{}' has no description", tool.name);
    ASSERT_MSG(static_cast<bool>(tool.handler), "MCP tool '{}' has no handler", tool.name);
    ASSERT_MSG(tool.input_schema.is_object() && tool.input_schema.value("type", "") == "object",
               "MCP tool '{}' input schema must be an object schema", tool.name);
    const auto [it, inserted] = tools.emplace(tool.name, std::move(tool));
    ASSERT_MSG(inserted, "duplicate MCP tool '{}'", it->first);
}

/// The tool with this name, or nullptr.
const Tool* ToolRegistry::Find(std::string_view name) const {
    const auto it = tools.find(name);
    return it == tools.end() ? nullptr : &it->second;
}

/// Definitions of all tools, in name order, for tools/list.
json ToolRegistry::ListJson() const {
    json list = json::array();
    for (const auto& [name, tool] : tools) {
        list.push_back(tool.DefinitionJson());
    }
    return list;
}

namespace {

/// Whether value has the JSON Schema type named by type. Integral floats such as 3.0 count as
/// integers; unknown type names always match.
bool MatchesType(const json& value, std::string_view type) {
    if (type == "object") {
        return value.is_object();
    }
    if (type == "array") {
        return value.is_array();
    }
    if (type == "string") {
        return value.is_string();
    }
    if (type == "boolean") {
        return value.is_boolean();
    }
    if (type == "integer") {
        if (value.is_number_integer()) {
            return true;
        }
        // No cast to an integer type here: that is undefined for values out of its range.
        const double d = value.is_number_float() ? value.get<double>() : 0.5;
        return std::isfinite(d) && std::trunc(d) == d && std::fabs(d) <= 9.007199254740992e15;
    }
    if (type == "number") {
        return value.is_number();
    }
    if (type == "null") {
        return value.is_null();
    }
    return true; // unknown keyword values are not enforced
}

} // namespace

/// Checks a value against a JSON Schema subset; returns the first error found, or nullopt.
std::optional<std::string> ValidateArguments(const json& schema, const json& value,
                                             const std::string& path) {
    if (!schema.is_object()) {
        return std::nullopt;
    }
    if (const auto type = schema.find("type"); type != schema.end()) {
        bool ok = false;
        if (type->is_string()) {
            ok = MatchesType(value, type->get<std::string>());
        } else if (type->is_array()) {
            for (const auto& t : *type) {
                ok = ok || (t.is_string() && MatchesType(value, t.get<std::string>()));
            }
        } else {
            ok = true;
        }
        if (!ok) {
            return fmt::format("{} must be of type {}", path, type->dump());
        }
    }
    if (const auto e = schema.find("enum"); e != schema.end() && e->is_array()) {
        if (std::find(e->begin(), e->end(), value) == e->end()) {
            return fmt::format("{} must be one of {}", path, e->dump());
        }
    }
    if (value.is_number()) {
        const double v = value.get<double>();
        if (const auto m = schema.find("minimum"); m != schema.end() && m->is_number() &&
                                                   v < m->get<double>()) {
            return fmt::format("{} must be >= {}", path, m->dump());
        }
        if (const auto m = schema.find("maximum"); m != schema.end() && m->is_number() &&
                                                   v > m->get<double>()) {
            return fmt::format("{} must be <= {}", path, m->dump());
        }
    }
    if (value.is_string()) {
        const size_t length = value.get_ref<const std::string&>().size();
        if (const auto m = schema.find("minLength"); m != schema.end() && m->is_number_integer() &&
                                                     length < m->get<size_t>()) {
            return fmt::format("{} must be at least {} characters", path, m->get<size_t>());
        }
        if (const auto m = schema.find("maxLength"); m != schema.end() && m->is_number_integer() &&
                                                     length > m->get<size_t>()) {
            return fmt::format("{} must be at most {} characters", path, m->get<size_t>());
        }
    }
    if (value.is_array()) {
        if (const auto m = schema.find("minItems"); m != schema.end() && m->is_number_integer() &&
                                                    value.size() < m->get<size_t>()) {
            return fmt::format("{} must have at least {} items", path, m->get<size_t>());
        }
        if (const auto m = schema.find("maxItems"); m != schema.end() && m->is_number_integer() &&
                                                    value.size() > m->get<size_t>()) {
            return fmt::format("{} must have at most {} items", path, m->get<size_t>());
        }
        if (const auto items = schema.find("items"); items != schema.end()) {
            for (size_t i = 0; i < value.size(); ++i) {
                if (auto error = ValidateArguments(*items, value[i], fmt::format("{}[{}]", path, i))) {
                    return error;
                }
            }
        }
    }
    if (value.is_object()) {
        const auto properties = schema.find("properties");
        if (const auto required = schema.find("required");
            required != schema.end() && required->is_array()) {
            for (const auto& name : *required) {
                if (name.is_string() && !value.contains(name.get<std::string>())) {
                    return fmt::format("{}.{} is required", path, name.get<std::string>());
                }
            }
        }
        const auto additional = schema.find("additionalProperties");
        const bool closed = additional != schema.end() && additional->is_boolean() &&
                            !additional->get<bool>();
        for (const auto& [key, item] : value.items()) {
            const bool known = properties != schema.end() && properties->is_object() &&
                               properties->contains(key);
            if (!known) {
                if (closed) {
                    return fmt::format("{}.{} is not a known property", path, key);
                }
                continue;
            }
            if (auto error = ValidateArguments((*properties)[key], item, path + "." + key)) {
                return error;
            }
        }
    }
    return std::nullopt;
}

} // namespace Mcp
