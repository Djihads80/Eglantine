// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Emulator settings by name (reflection over Settings::values.linkage), debug knobs, determinism
// presets and the NX2 env switches in effect. Web-service credentials and the MCP server's own
// security settings are never exposed or changed.

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <fmt/format.h>

#include "common/settings.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

#ifdef _WIN32
#include <stdlib.h> // _environ
#else
extern char** environ;
#endif

namespace Mcp {

namespace {

/// ASCII lower-case copy of s, for case-insensitive matching.
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Settings agents may not read or change: credentials, console identity (serials), and the MCP
/// server's own access control.
bool Hidden(const Settings::BasicSetting& s) {
    const std::string name = Lower(s.GetLabel());
    return s.GetCategory() == Settings::Category::WebService || name.starts_with("mcp_") ||
           name == "enable_mcp_server" || name.find("token") != std::string::npos ||
           name.find("password") != std::string::npos || name.find("serial") != std::string::npos;
}

/// Looks up a setting by its exact config key. Returns nullptr if unknown or Hidden.
Settings::BasicSetting* Find(const std::string& name) {
    auto& by_key = Settings::values.linkage.by_key;
    const auto it = by_key.find(name);
    if (it == by_key.end() || Hidden(*it->second)) {
        return nullptr;
    }
    return it->second;
}

/// Name and value of a setting (enum values by name). With detail, also its category, default,
/// type, runtime modifiability, raw enum index, range and per-game override state.
json SettingJson(const Settings::BasicSetting& s, bool detail) {
    json out{{"name", s.GetLabel()}, {"value", s.IsEnum() ? s.Canonicalize() : s.ToString()}};
    if (detail) {
        out["category"] = Settings::TranslateCategory(s.GetCategory());
        out["default"] = s.DefaultToString();
        out["type"] = std::string{s.TypeId()};
        out["runtime_modifiable"] = s.RuntimeModifiable();
        if (s.IsEnum()) {
            out["raw"] = s.ToString();
        }
        if (s.Ranged()) {
            out["min"] = s.MinVal();
            out["max"] = s.MaxVal();
        }
        if (s.Switchable()) {
            out["per_game_override"] = !s.UsingGlobal();
        }
    }
    return out;
}

/// Sets a setting from text; enum settings also accept their names (LoadString takes indices).
std::string SetFromString(Settings::BasicSetting& s, const std::string& value) {
    const std::string before = s.ToString();
    if (s.IsEnum() && !value.empty() && !std::isdigit(static_cast<unsigned char>(value[0]))) {
        for (int i = 0; i < 64; ++i) {
            s.LoadString(std::to_string(i));
            if (Lower(s.Canonicalize()) == Lower(value)) {
                return {};
            }
        }
        s.LoadString(before);
        return fmt::format("'{}' is not a value of {}", value, s.GetLabel());
    }
    s.LoadString(value);
    return {};
}

/// settings_list: visible settings, optionally limited to one category (case-insensitive), a
/// name substring, or values that differ from their default.
ToolResult SettingsList(ToolContext&, const json& args) {
    const std::string filter = Lower(args.value("filter", ""));
    const std::string category = Lower(args.value("category", ""));
    const bool changed_only = args.value("changed_only", false);
    json list = json::array();
    for (const auto& [cat, settings] : Settings::values.linkage.by_category) {
        const std::string cat_name = Lower(Settings::TranslateCategory(cat));
        if (!category.empty() && cat_name != category) {
            continue;
        }
        for (const auto* s : settings) {
            if (Hidden(*s) || (!filter.empty() && Lower(s->GetLabel()).find(filter) == std::string::npos)) {
                continue;
            }
            if (changed_only && s->ToString() == s->DefaultToString()) {
                continue;
            }
            json entry = SettingJson(*s, false);
            entry["category"] = Settings::TranslateCategory(cat);
            list.push_back(std::move(entry));
        }
    }
    return ToolResult::Structured(json{{"settings", std::move(list)}});
}

/// settings_get: detailed SettingJson per requested name; unknown or hidden names get an error
/// entry instead of failing the call.
ToolResult SettingsGet(ToolContext&, const json& args) {
    json out = json::array();
    for (const auto& n : args.at("names")) {
        const auto name = n.get<std::string>();
        if (auto* s = Find(name)) {
            out.push_back(SettingJson(*s, true));
        } else {
            out.push_back({{"name", name}, {"error", "unknown or hidden setting"}});
        }
    }
    return ToolResult::Structured(json{{"settings", std::move(out)}});
}

/**
 * Sets each name -> value pair in changes on the calling thread, logging each change on the
 * timeline. Failures are reported per entry and do not stop the others.
 * @param apply When true and a game is running, asks the host to apply the new settings.
 * @return Per-setting before/after or error, with reboot_needed if a changed setting is not
 *         runtime-modifiable. is_error is set only when a single requested change failed.
 */
ToolResult ApplyChanges(ToolContext& ctx, const json& changes, bool apply) {
    json results = json::array();
    bool any_error = false;
    bool needs_reboot = false;
    for (const auto& [name, value] : changes.items()) {
        auto* s = Find(name);
        if (!s) {
            results.push_back({{"name", name}, {"error", "unknown or hidden setting"}});
            any_error = true;
            continue;
        }
        const std::string before = s->IsEnum() ? s->Canonicalize() : s->ToString();
        const std::string text = value.is_string() ? value.get<std::string>()
                                 : value.is_boolean() ? (value.get<bool>() ? "true" : "false")
                                                      : value.dump();
        if (std::string error = SetFromString(*s, text); !error.empty()) {
            results.push_back({{"name", name}, {"error", error}});
            any_error = true;
            continue;
        }
        json entry{{"name", name},
                   {"before", before},
                   {"after", s->IsEnum() ? s->Canonicalize() : s->ToString()}};
        if (ctx.GameRunning() && !s->RuntimeModifiable()) {
            entry["note"] = "not runtime-modifiable: takes effect on the next boot";
            needs_reboot = true;
        }
        results.push_back(std::move(entry));
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "mcp",
                               fmt::format("setting {}: {} -> {}", name, before,
                                           results.back()["after"].get<std::string>()));
        }
    }
    if (apply && ctx.host && ctx.GameRunning()) {
        ctx.host->ApplySettings();
    }
    json out{{"results", std::move(results)},
             {"note", "Changed in memory; the emulator may save them to its config when it saves "
                      "settings (e.g. on exit)."}};
    if (needs_reboot) {
        out["reboot_needed"] = true;
    }
    ToolResult r = ToolResult::Structured(std::move(out));
    // Any failed entry fails the call; the per-entry results say which ones applied.
    r.is_error = any_error;
    return r;
}

ToolResult SettingsSet(ToolContext& ctx, const json& args) {
    return ApplyChanges(ctx, args.at("values"), true);
}

/// debug_knobs: applies 'value', then 'set' and 'clear' bit lists, to the 16-bit knob mask.
/// Reading is always allowed; an actual change needs MCP writes enabled.
ToolResult DebugKnobs(ToolContext& ctx, const json& args) {
    auto& knobs = Settings::values.debug_knobs;
    const u16 before = knobs.GetValue();
    u16 value = before;
    if (args.contains("value")) {
        value = static_cast<u16>(args["value"].get<u32>());
    }
    for (const auto& b : args.value("set", json::array())) {
        value |= static_cast<u16>(1u << (b.get<u32>() & 0xF));
    }
    for (const auto& b : args.value("clear", json::array())) {
        value &= static_cast<u16>(~(1u << (b.get<u32>() & 0xF)));
    }
    if (value != before) {
        if (!ctx.writes_allowed) {
            return ToolResult::Error("changing knobs needs MCP writes (Tools > MCP Server > Allow Writes)");
        }
        // Reading is free; changing needs the control lease like the other state changes.
        ControlLease::Holder holder;
        if (ctx.control != nullptr && !ctx.session_id.empty() &&
            !ctx.control->Touch(ctx.session_id, holder)) {
            return ToolResult::Error("another agent holds the control lease (control_status)");
        }
        knobs.SetValue(value);
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "mcp",
                               fmt::format("debug_knobs {:#06x} -> {:#06x}", before, value));
        }
    }
    json bits = json::array();
    for (u32 i = 0; i < 16; ++i) {
        if ((value >> i) & 1) {
            bits.push_back(i);
        }
    }
    return ToolResult::Structured(json{{"value", fmt::format("{:#06x}", value)},
                                       {"previous", fmt::format("{:#06x}", before)},
                                       {"bits_on", std::move(bits)},
                                       {"note", "Code reads them with Settings::GetDebugKnobAt(i): "
                                                "wrap an experimental path in a knob to A/B it "
                                                "live without rebuilding."}});
}

/// set_determinism: maps rng_seed, rtc, language and region onto their settings (enabling the
/// seed/RTC overrides) and applies them via ApplyChanges. clear wins over rng_seed and rtc.
ToolResult SetDeterminism(ToolContext& ctx, const json& args) {
    json changes = json::object();
    if (args.contains("rng_seed")) {
        changes["rng_seed_enabled"] = true;
        changes["rng_seed"] = args["rng_seed"].get<u32>();
    }
    if (args.contains("rtc")) {
        changes["custom_rtc_enabled"] = true;
        changes["custom_rtc"] = args["rtc"].get<s64>();
    }
    if (args.value("clear", false)) {
        changes["rng_seed_enabled"] = false;
        changes["custom_rtc_enabled"] = false;
    }
    if (args.contains("language")) {
        changes["language_index"] = args["language"];
    }
    if (args.contains("region")) {
        changes["region_index"] = args["region"];
    }
    if (changes.empty()) {
        return ToolResult::Error("pass rng_seed, rtc, language, region or clear");
    }
    return ApplyChanges(ctx, changes, true);
}

/// env_switches: the emulator's own EDEN_* environment variables, optionally narrowed by a longer
/// prefix. Other variables are never returned (the environment can hold unrelated credentials).
ToolResult EnvSwitches(ToolContext&, const json& args) {
    const std::string prefix = args.value("prefix", "EDEN_");
    if (!prefix.starts_with("EDEN_")) {
        return ToolResult::Error("prefix must start with EDEN_ (only the emulator's own "
                                 "switches are listed)");
    }
    json vars = json::object();
#ifdef _WIN32
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (; env != nullptr && *env != nullptr; ++env) {
        const std::string_view entry{*env};
        if (!entry.starts_with(prefix)) {
            continue;
        }
        const auto eq = entry.find('=');
        vars[std::string{entry.substr(0, eq)}] =
            eq == std::string_view::npos ? "" : std::string{entry.substr(eq + 1)};
    }
    return ToolResult::Structured(json{
        {"variables", std::move(vars)},
        {"note", "Most EDEN_NX2_* switches are read once at first use: changing them needs a "
                 "relaunch with the variable set (the GPU profiler is live: gpu_profile)."}});
}

} // namespace

Settings::BasicSetting* FindAgentSetting(const std::string& name) {
    return Find(name);
}

std::string SetSettingFromText(Settings::BasicSetting& setting, const std::string& value) {
    return SetFromString(setting, value);
}

/// Registers the settings, debug knob, determinism and environment switch tools.
void RegisterSettingsTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "settings_list",
        .title = "List settings",
        .description = "Emulator settings with their current values, by category. filter matches "
                       "names; changed_only lists only non-default ones (a quick 'what's unusual "
                       "here'). Credentials and MCP security settings are hidden.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"filter", {{"type", "string"}}},
                           {"category", {{"type", "string"}, {"description", "e.g. Renderer, Core, System, Debugging"}}},
                           {"changed_only", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = SettingsList,
    });
    registry.Add(Tool{
        .name = "settings_get",
        .title = "Get settings",
        .description = "Details of settings by name: value, default, type, range, whether they can "
                       "change while a game runs, and per-game overrides.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"names", {{"type", "array"}, {"items", {{"type", "string"}}},
                                      {"minItems", 1}, {"maxItems", 100}}}}},
                         {"required", {"names"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = SettingsGet,
    });
    registry.Add(Tool{
        .name = "settings_set",
        .title = "Change settings",
        .description = "Set settings by name, e.g. {values: {\"resolution_setup\": \"Res1X\", "
                       "\"use_vsync\": \"Fifo\"}}. Enums take their names or indices. Applied to "
                       "the running game where possible; others report reboot_needed.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"values", {{"type", "object"},
                                       {"description", "Setting name -> new value."}}}}},
                         {"required", {"values"}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = SettingsSet,
    });
    registry.Add(Tool{
        .name = "debug_knobs",
        .title = "Debug knobs",
        .description = "Read or change the 16 debug knob bits (Settings::GetDebugKnobAt) that "
                       "experimental code paths can check, to A/B a change live. set / clear take "
                       "bit numbers, value replaces all. Changing needs writes.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"value", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65535}}},
                           {"set", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}}}},
                           {"clear", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}}}}}},
                         {"additionalProperties", false}},
        .handler = DebugKnobs,
    });
    registry.Add(Tool{
        .name = "set_determinism",
        .title = "Determinism settings",
        .description = "Make runs repeatable: fixed RNG seed, fixed RTC (unix seconds), language "
                       "and region (names or indices); clear:true turns the seed and RTC back off. "
                       "Most take effect on the next boot.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"rng_seed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295LL}}},
                           {"rtc", {{"type", "integer"}}},
                           {"language", {{"type", "string"}, {"description", "e.g. EnglishAmerican, Japanese, or an index"}}},
                           {"region", {{"type", "string"}, {"description", "e.g. Usa, Europe, Japan, or an index"}}},
                           {"clear", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = SetDeterminism,
    });
    registry.Add(Tool{
        .name = "env_switches",
        .title = "Environment switches",
        .description = "The emulator's EDEN_* environment variables, i.e. which EDEN_NX2_* "
                       "diagnostics and workarounds are on. prefix narrows the list and must start "
                       "with EDEN_; other variables are never returned.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"prefix", {{"type", "string"}, {"pattern", "^EDEN_"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = EnvSwitches,
    });
}

} // namespace Mcp
