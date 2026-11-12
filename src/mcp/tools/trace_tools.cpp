// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// IPC / SVC tracing into the timeline (runtime switches instead of the EDEN_NX2_*_TRACE env
// vars), an IPC call summary, and newly seen unimplemented commands as timeline events.

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

#include <fmt/format.h>

#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debugger/trace_hooks.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/unimplemented_registry.h"
#include "mcp/context.h"
#include "mcp/server.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// Filters and counters of the IPC and SVC traces, shared between the tool handlers and the hooks
/// (which run on emulator threads). Every field is guarded by mutex.
struct TraceState {
    std::mutex mutex;
    // IPC
    std::string ipc_prefix;
    bool ipc_errors_only = false;
    bool ipc_events = true; ///< false: count for ipc_summary only
    std::map<std::tuple<std::string, u32, std::string, u32>, u64> ipc_counts;
    u64 ipc_total = 0;
    // SVC
    std::set<u32> svc_filter; ///< empty = all
    std::map<u32, u64> svc_counts;
};

/// The process-wide trace state, created on first use.
TraceState& State() {
    static TraceState state;
    return state;
}

/// IPC hook, called on the emulator thread that dispatched the command. Counts calls matching the
/// service prefix, then (outside the lock) posts an 'ipc' event with up to 12 request words unless
/// events are off or errors_only filters it out.
void OnIpc(const Core::Trace::IpcCall& call) {
    auto& s = State();
    bool emit = false;
    {
        std::scoped_lock lock{s.mutex};
        if (!s.ipc_prefix.empty() && !call.service.starts_with(s.ipc_prefix)) {
            return;
        }
        ++s.ipc_counts[{std::string{call.service}, call.command, std::string{call.function},
                        call.result}];
        ++s.ipc_total;
        emit = s.ipc_events && (!s.ipc_errors_only || call.result != 0);
    }
    Timeline* timeline = Server::ActiveTimeline();
    if (!emit || !timeline) {
        return;
    }
    std::string words;
    for (size_t i = 0; i < std::min<size_t>(call.request.size(), 12); ++i) {
        words += fmt::format("{}{:08x}", i ? " " : "", call.request[i]);
    }
    timeline->Push(EventKind::Ipc, 0, std::string{call.service},
                   fmt::format("{}{} {} -> {:#x} t{} req [{}]", call.command,
                               call.type >= 16 ? " (tipc)" : "", call.function, call.result,
                               call.thread_id, words));
}

/// SVC hook, called on the guest thread making the call. Counts SVCs that pass the id filter and
/// posts each as an 'svc' event with the first four inputs and three outputs.
void OnSvc(const Core::Trace::SvcCall& call) {
    auto& s = State();
    {
        std::scoped_lock lock{s.mutex};
        if (!s.svc_filter.empty() && !s.svc_filter.contains(call.imm)) {
            return;
        }
        ++s.svc_counts[call.imm];
    }
    if (Timeline* timeline = Server::ActiveTimeline()) {
        timeline->Push(EventKind::Svc, 0, fmt::format("svc{:02x}", call.imm),
                       fmt::format("t{} in [{:#x} {:#x} {:#x} {:#x}] out [{:#x} {:#x} {:#x}]",
                                   call.thread_id, call.in[0], call.in[1], call.in[2], call.in[3],
                                   call.out[0], call.out[1], call.out[2]));
    }
}

/// Posts a newly seen unimplemented command as an 'unimpl' event, with its first request words
/// and backtrace.
void OnUnimplemented(const Service::UnimplementedCommandRecord& r) {
    if (Timeline* timeline = Server::ActiveTimeline()) {
        timeline->Push(EventKind::Unimplemented, 0, r.service,
                       fmt::format("cmd {}{}{} req {}\n{}", r.command,
                                   r.function.empty() ? "" : " " + r.function,
                                   r.auto_stubbed ? " (auto-stubbed)" : "", r.first_request,
                                   r.first_backtrace));
    }
}

/// Posts a guest crash as a critical-level 'crash' event, adding the faulting thread's id, name and
/// backtrace when a thread is given. Does nothing when no server timeline is active.
void OnCrash(std::string_view kind, std::string_view detail, const Kernel::KThread* thread) {
    Timeline* timeline = Server::ActiveTimeline();
    if (!timeline) {
        return;
    }
    std::string text{detail};
    if (thread != nullptr) {
        text += fmt::format("\nthread {} ({})", thread->GetThreadId(),
                            Core::GetThreadName(thread).value_or("?"));
        for (const auto& f : Core::GetBacktrace(thread)) {
            text += fmt::format("\n{}+{:#x} {}", f.module, f.offset, f.name);
        }
    }
    timeline->Push(EventKind::Crash, 5, std::string{kind}, std::move(text));
}

/// ipc_trace: stores the filter options and installs or removes the IPC hook. Enabling resets the
/// counters; disabling keeps them for ipc_summary.
ToolResult IpcTrace(ToolContext&, const json& args) {
    auto& s = State();
    const bool enable = args.at("enable").get<bool>();
    {
        std::scoped_lock lock{s.mutex};
        s.ipc_prefix = args.value("service_prefix", "");
        s.ipc_errors_only = args.value("errors_only", false);
        s.ipc_events = args.value("events", true);
        if (enable) {
            s.ipc_counts.clear();
            s.ipc_total = 0;
        }
    }
    Core::Trace::g_ipc_hook.store(enable ? &OnIpc : nullptr, std::memory_order_relaxed);
    return ToolResult::Structured(json{
        {"enabled", enable},
        {"note", enable ? "Each dispatched IPC command becomes an 'ipc' timeline event (service, "
                          "command, handler, result, thread, request words). ipc_summary has "
                          "counts. Busy games make thousands a second: filter with "
                          "service_prefix / errors_only, or events:false to only count."
                        : "IPC tracing off."}});
}

/// ipc_summary: copies the IPC counters under the lock (optionally clearing them), then returns the
/// "top" most frequent (service, command, function, result) rows. "kinds" is the number of rows.
ToolResult IpcSummary(ToolContext&, const json& args) {
    auto& s = State();
    std::vector<std::pair<u64, json>> rows;
    u64 total = 0;
    {
        std::scoped_lock lock{s.mutex};
        total = s.ipc_total;
        for (const auto& [key, count] : s.ipc_counts) {
            const auto& [service, command, function, result] = key;
            rows.emplace_back(count, json{{"service", service},
                                          {"command", command},
                                          {"function", function},
                                          {"result", fmt::format("{:#x}", result)},
                                          {"count", count}});
        }
        if (args.value("reset", false)) {
            s.ipc_counts.clear();
            s.ipc_total = 0;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t top = args.value("top", size_t{40});
    json list = json::array();
    for (size_t i = 0; i < std::min(top, rows.size()); ++i) {
        list.push_back(std::move(rows[i].second));
    }
    return ToolResult::Structured(json{
        {"tracing", Core::Trace::g_ipc_hook.load() != nullptr},
        {"total_calls", total},
        {"kinds", rows.size()},
        {"top", std::move(list)}});
}

/// svc_trace: replaces the SVC id filter and installs or removes the SVC hook. Enabling resets the
/// counters; the reply carries the current per-SVC counts.
ToolResult SvcTrace(ToolContext&, const json& args) {
    auto& s = State();
    const bool enable = args.at("enable").get<bool>();
    {
        std::scoped_lock lock{s.mutex};
        s.svc_filter.clear();
        for (const auto& id : args.value("svcs", json::array())) {
            s.svc_filter.insert(id.get<u32>());
        }
        if (enable) {
            s.svc_counts.clear();
        }
    }
    Core::Trace::g_svc_hook.store(enable ? &OnSvc : nullptr, std::memory_order_relaxed);
    json counts = json::object();
    {
        std::scoped_lock lock{s.mutex};
        for (const auto& [imm, count] : s.svc_counts) {
            counts[fmt::format("{:#04x}", imm)] = count;
        }
    }
    return ToolResult::Structured(json{
        {"enabled", enable},
        {"counts_since_enabled", std::move(counts)},
        {"note", "SVC events go to the timeline as kind 'svc' (source svcXX). Unfiltered tracing "
                 "can be tens of thousands a second and overruns the timeline: pass svcs, e.g. "
                 "[0x18] for WaitSynchronization."}});
}

/// list_services: sorted registered service names matching the prefix. "total" counts all
/// registered services, not just the matches.
ToolResult ListServices(ToolContext& ctx, const json& args) {
    auto names = ctx.system->ServiceManager().GetRegisteredServiceNames();
    std::sort(names.begin(), names.end());
    const std::string prefix = args.value("prefix", "");
    json list = json::array();
    for (const auto& name : names) {
        if (prefix.empty() || name.starts_with(prefix)) {
            list.push_back(name);
        }
    }
    return ToolResult::Structured(json{{"services", std::move(list)}, {"total", names.size()}});
}

} // namespace

void InstallTraceHooks() {
    Service::g_unimplemented_hook.store(&OnUnimplemented, std::memory_order_release);
    Core::Trace::g_crash_hook.store(&OnCrash, std::memory_order_release);
}

void RemoveTraceHooks() {
    Service::g_unimplemented_hook.store(nullptr, std::memory_order_release);
    Core::Trace::g_crash_hook.store(nullptr, std::memory_order_release);
    Core::Trace::g_ipc_hook.store(nullptr, std::memory_order_relaxed);
    Core::Trace::g_svc_hook.store(nullptr, std::memory_order_relaxed);
}

/// Registers ipc_trace, ipc_summary, svc_trace and list_services.
void RegisterTraceTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "ipc_trace",
        .title = "Trace IPC calls",
        .description = "Turn IPC (service command) tracing on or off at runtime. Each call becomes "
                       "an 'ipc' timeline event with service, command id, handler name, result "
                       "word, calling thread and request words; ipc_summary aggregates counts. "
                       "Newly seen unimplemented commands are always posted as 'unimpl' events "
                       "(with the caller's backtrace), tracing or not.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"enable", {{"type", "boolean"}}},
                           {"service_prefix", {{"type", "string"}}},
                           {"errors_only", {{"type", "boolean"},
                                            {"description", "Only calls whose result is non-zero."}}},
                           {"events", {{"type", "boolean"},
                                       {"description", "false = count only (ipc_summary), no events."}}}}},
                         {"required", {"enable"}},
                         {"additionalProperties", false}},
        .handler = IpcTrace,
    });
    registry.Add(Tool{
        .name = "ipc_summary",
        .title = "IPC call summary",
        .description = "IPC calls counted since ipc_trace was enabled, grouped by service, "
                       "command and result, most frequent first (finds a game polling something "
                       "that never becomes ready, or a command failing repeatedly).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"top", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
                           {"reset", {{"type", "boolean"},
                                      {"description", "Clear the counters after reading them."}}}}},
                         {"additionalProperties", false}},
        // reset clears the MCP's own counters (no emulator state), so no Allow Writes needed.
        .annotations = {},
        .handler = IpcSummary,
    });
    registry.Add(Tool{
        .name = "svc_trace",
        .title = "Trace SVCs",
        .description = "Turn supervisor-call tracing on or off at runtime: each SVC (optionally only "
                       "the ids in svcs) becomes an 'svc' timeline event with thread, inputs and "
                       "outputs. Returns per-SVC counts since it was enabled.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"enable", {{"type", "boolean"}}},
                           {"svcs", {{"type", "array"},
                                     {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}},
                                     {"maxItems", 64}}}}},
                         {"required", {"enable"}},
                         {"additionalProperties", false}},
        .handler = SvcTrace,
    });
    registry.Add(Tool{
        .name = "list_services",
        .title = "List HLE services",
        .description = "Names of the services the emulator registered (what a game can connect "
                       "to), optionally filtered by prefix. Switch 2 services the emulator lacks "
                       "get stub sessions on demand and are not listed; ipc_summary shows those "
                       "in use.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"prefix", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = ListServices,
    });
}

} // namespace Mcp
