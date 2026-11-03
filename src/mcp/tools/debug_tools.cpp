// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Debugging / bring-up tools.

#include <algorithm>

#include <fmt/format.h>

#include "core/hle/service/unimplemented_registry.h"
#include "mcp/context.h"
#include "mcp/tool_registry.h"

namespace Mcp {

namespace {

/// unimplemented_commands: lists the recorded unimplemented service commands, optionally filtered
/// by service prefix and sorted by call count. "total" counts the filtered list. With "reset" the
/// whole registry is cleared after the list is built, including entries the filter excluded.
ToolResult UnimplementedCommands(ToolContext&, const json& args) {
    auto records = Service::GetUnimplementedCommands();
    const std::string service_filter = args.value("service_prefix", "");
    if (args.value("sort", "first_seen") == "count") {
        std::stable_sort(records.begin(), records.end(),
                         [](const auto& a, const auto& b) { return a.count > b.count; });
    }
    json list = json::array();
    for (const auto& r : records) {
        if (!service_filter.empty() && !r.service.starts_with(service_filter)) {
            continue;
        }
        json item{{"service", r.service},
                  {"command", r.command},
                  {"count", r.count},
                  {"auto_stubbed", r.auto_stubbed},
                  {"first_seen_s", static_cast<double>(r.first_seen_us) / 1e6},
                  {"first_request", r.first_request}};
        if (!r.function.empty()) {
            item["function"] = r.function;
        }
        if (args.value("backtraces", true) && !r.first_backtrace.empty()) {
            item["first_backtrace"] = r.first_backtrace;
        }
        list.push_back(std::move(item));
    }
    const size_t total = list.size();
    if (args.value("reset", false)) {
        Service::ClearUnimplementedCommands();
    }
    return ToolResult::Structured(json{
        {"commands", std::move(list)},
        {"total", total},
        {"note", "Every call to a service command without an implementation since boot (or the "
                 "last reset). auto_stubbed ones got an empty success reply, which games may "
                 "misread: these are candidates for real implementations. first_request holds the "
                 "first call's command words for inferring arguments."},
    });
}

} // namespace

/// Registers the bring-up debugging tools.
void RegisterDebugTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "unimplemented_commands",
        .title = "Unimplemented service commands",
        .description = "Service (IPC) commands the running software called that the emulator does "
                       "not implement, aggregated with call counts, whether they were auto-stubbed, "
                       "and the first call's request words. The starting point for working out what "
                       "a Switch 2 stub should return.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"service_prefix",
                            {{"type", "string"}, {"description", "e.g. 'nx2:' or 'appletOE'"}}},
                           {"sort", {{"type", "string"}, {"enum", {"first_seen", "count"}}}},
                           {"backtraces", {{"type", "boolean"},
                                           {"description", "Include the caller's backtrace at the "
                                                           "first call (default true)."}}},
                           {"reset", {{"type", "boolean"},
                                      {"description", "Clear the list after returning it."}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = false, .idempotent = false},
        .handler = UnimplementedCommands,
    });
}

} // namespace Mcp
