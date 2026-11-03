// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Phase 1 tools: emulator status and the event timeline.

#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/server.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

/// An event as JSON; log events also get their level name.
json EventJson(const Event& e) {
    json out{{"seq", e.seq},
             {"t_ms", static_cast<double>(e.time_us) / 1000.0},
             {"frame", e.frame},
             {"kind", EventKindName(e.kind)},
             {"source", e.source},
             {"msg", e.message}};
    if (e.kind == EventKind::Log) {
        static constexpr const char* levels[] = {"trace", "debug", "info",
                                                 "warning", "error", "critical"};
        out["level"] = e.level < std::size(levels) ? levels[e.level] : "?";
    }
    return out;
}

/// Shared filter arguments for timeline and wait_for.
json FilterProperties() {
    return json{
        {"since_seq",
         {{"type", "integer"},
          {"minimum", 0},
          {"description", "Only events with seq greater than this (use the previous next_seq)."}}},
        {"kinds",
         {{"type", "array"},
          {"items",
           {{"type", "string"},
            {"enum", {"log", "state", "input", "mark", "ipc", "svc", "unimpl", "watch", "crash",
                      "frame", "break"}}}},
          {"description", "Event kinds to include (default: all)."}}},
        {"min_level",
         {{"type", "string"},
          {"enum", {"trace", "debug", "info", "warning", "error", "critical"}},
          {"description", "Minimum level for log events."}}},
        {"source_prefix",
         {{"type", "string"},
          {"description", "Source prefix, e.g. a log class like 'Service.NVDRV' or 'Render'."}}},
        {"contains", {{"type", "string"}, {"description", "Substring the message must contain."}}},
    };
}

/// Builds a timeline query from the shared filter arguments.
Timeline::Query ParseQuery(const json& args) {
    Timeline::Query q;
    q.since_seq = args.value("since_seq", u64{0});
    if (args.contains("kinds")) {
        for (const auto& k : args["kinds"]) {
            if (auto kind = EventKindFromName(k.get<std::string>())) {
                q.kinds.push_back(*kind);
            }
        }
    }
    if (args.contains("min_level")) {
        static constexpr std::string_view levels[] = {"trace", "debug", "info",
                                                      "warning", "error", "critical"};
        const auto name = args["min_level"].get<std::string>();
        for (int i = 0; i < static_cast<int>(std::size(levels)); ++i) {
            if (levels[i] == name) {
                q.min_level = i;
            }
        }
    }
    q.source_prefix = args.value("source_prefix", "");
    q.contains = args.value("contains", "");
    return q;
}

/// FPS, speed, frame time and shader builds from the host, or null if unavailable.
json PerfJson(ToolContext& ctx) {
    if (!ctx.host) {
        return nullptr;
    }
    const PerfSnapshot perf = ctx.host->GetPerfSnapshot();
    if (!perf.valid) {
        return nullptr;
    }
    return json{{"game_fps", perf.game_fps},
                {"speed_pct", perf.emulation_speed * 100.0},
                {"frametime_ms", perf.frametime_ms},
                {"shaders_building", perf.shaders_building}};
}

u64 PresentedFrames() {
    return Core::g_nx2_present_count.load(std::memory_order_relaxed);
}

/// Waits until `frames` more frames are presented, the timeout or a server stop.
/// Returns the number of frames actually presented.
u64 WaitFrames(u64 frames, std::chrono::milliseconds timeout, std::stop_token stop) {
    const u64 start = PresentedFrames();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (PresentedFrames() - start < frames && std::chrono::steady_clock::now() < deadline &&
           !ServerStopping() && !stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return PresentedFrames() - start;
}

namespace {

/// Lowercase hex string of the bytes, two digits each, no separators.
std::string HexBytes(std::span<const u8> bytes) {
    std::string out;
    for (const u8 b : bytes) {
        out += fmt::format("{:02x}", b);
    }
    return out;
}

/// status: frontend and build version, MCP state, and when a game runs its ids, title, frame and
/// docked mode. Perf is added whenever the host has a valid sample.
ToolResult Status(ToolContext& ctx, const json&) {
    json out;
    out["frontend"] = ctx.host ? ctx.host->FrontendName() : "none";
    // The scm_rev globals are arrays of unknown bound, which json can't deduce: make strings.
    out["version"] = {{"build", std::string{Common::g_build_fullname}},
                      {"rev", std::string{Common::g_scm_rev}},
                      {"branch", std::string{Common::g_scm_branch}}};
    out["mcp"] = {{"writes_enabled", ctx.writes_allowed},
                  {"timeline_last_seq", ctx.timeline ? ctx.timeline->LastSeq() : 0}};
    const bool running = ctx.GameRunning();
    out["powered_on"] = running;
    if (running) {
        auto& system = *ctx.system;
        out["paused"] = system.IsPaused();
        out["program_id"] = fmt::format("{:016X}", system.GetApplicationProcessProgramID());
        std::string title;
        if (system.GetGameName(title) == Loader::ResultStatus::Success) {
            out["title"] = title;
        }
        out["build_id"] = HexBytes(system.GetApplicationProcessBuildID());
        out["frame"] = PresentedFrames();
        out["docked"] = Settings::IsDockedMode();
    }
    if (json perf = PerfJson(ctx); !perf.is_null()) {
        out["perf"] = std::move(perf);
    }
    return ToolResult::Structured(std::move(out));
}

/// timeline: returns up to "limit" matching events after since_seq, with next_seq to continue
/// from, and "dropped"/"truncated" only when non-zero.
ToolResult ReadTimeline(ToolContext& ctx, const json& args) {
    if (!ctx.timeline) {
        return ToolResult::Error("timeline unavailable");
    }
    Timeline::Query q = ParseQuery(args);
    q.limit = args.value("limit", size_t{200});
    const auto r = ctx.timeline->Read(q);
    json events = json::array();
    for (const auto& e : r.events) {
        events.push_back(EventJson(e));
    }
    json out{{"events", std::move(events)}, {"next_seq", r.next_seq}};
    if (r.dropped) {
        out["dropped"] = r.dropped;
    }
    if (r.truncated) {
        out["truncated"] = true;
    }
    return ToolResult::Structured(std::move(out));
}

/// wait_for: blocks the HTTP worker thread handling the request until a matching event arrives
/// or the timeout passes. Without since_seq only events pushed after the call starts count. On a
/// match next_seq is that event's seq; on timeout it is the last seq and "matched" is false.
ToolResult WaitFor(ToolContext& ctx, const json& args) {
    if (!ctx.timeline) {
        return ToolResult::Error("timeline unavailable");
    }
    Timeline::Query q = ParseQuery(args);
    if (!args.contains("since_seq")) {
        q.since_seq = ctx.timeline->LastSeq(); // only events from now on
    }
    const auto timeout = std::chrono::milliseconds{args.value("timeout_ms", 10000)};
    const auto found = ctx.timeline->WaitFor(q, timeout);
    if (!found) {
        return ToolResult::Structured(
            json{{"matched", false}, {"next_seq", ctx.timeline->LastSeq()}});
    }
    return ToolResult::Structured(
        json{{"matched", true}, {"event", EventJson(*found)}, {"next_seq", found->seq}});
}

/// mark: pushes an agent marker event and returns its seq.
ToolResult Mark(ToolContext& ctx, const json& args) {
    if (!ctx.timeline) {
        return ToolResult::Error("timeline unavailable");
    }
    const u64 seq =
        ctx.timeline->Push(EventKind::Mark, 0, "agent", args.at("label").get<std::string>());
    return ToolResult::Structured(json{{"seq", seq}});
}

/// set_log_filter: applies the filter string to the global logger and stores it in the settings
/// value. Returns the new and previous filter strings; parse errors are not reported.
ToolResult SetLogFilter(ToolContext&, const json& args) {
    const std::string filter_string = args.at("filter").get<std::string>();
    const std::string previous = Settings::values.log_filter.GetValue();
    Common::Log::Filter filter;
    filter.ParseFilterString(filter_string);
    Common::Log::SetGlobalFilter(filter);
    Settings::values.log_filter.SetValue(filter_string);
    return ToolResult::Structured(json{{"filter", filter_string}, {"previous", previous}});
}

} // namespace

/// Registers status, timeline, wait_for, mark and set_log_filter.
void RegisterCoreTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "status",
        .title = "Emulator status",
        .description = "Whether a game is running or paused, its program id, title and build id, "
                       "the current guest frame, performance, emulator version, and whether MCP "
                       "writes are enabled. Call this first.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = Status,
    });

    json timeline_props = FilterProperties();
    timeline_props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000},
                               {"description", "Max events to return (default 200)."}};
    registry.Add(Tool{
        .name = "timeline",
        .title = "Read the event timeline",
        .description =
            "Everything the emulator recorded (log lines, state changes, agent marks, and later "
            "IPC/SVC/memory-watch events), oldest first, each with seq, time, guest frame and "
            "source. Pass the returned next_seq back as since_seq to continue without gaps. "
            "'dropped' reports events overwritten before you read them.",
        .input_schema = {{"type", "object"},
                         {"properties", timeline_props},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = ReadTimeline,
    });

    json wait_props = FilterProperties();
    wait_props["timeout_ms"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 120000},
                                {"description", "How long to wait (default 10000)."}};
    registry.Add(Tool{
        .name = "wait_for",
        .title = "Wait for an event",
        .description = "Block until a timeline event matching the filters occurs (by default only "
                       "events after this call starts), or until timeout_ms. Use this instead of "
                       "polling, e.g. wait for a log line, a crash or a state change.",
        .input_schema = {{"type", "object"}, {"properties", wait_props}, {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .handler = WaitFor,
    });

    registry.Add(Tool{
        .name = "mark",
        .title = "Add a timeline marker",
        .description = "Insert a labelled marker into the timeline to bracket an experiment "
                       "(e.g. 'before pressing A'). Returns its seq.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"label", {{"type", "string"}, {"minLength", 1}, {"maxLength", 500}}}}},
                         {"required", {"label"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = false, .idempotent = false},
        .handler = Mark,
    });

    registry.Add(Tool{
        .name = "set_log_filter",
        .title = "Set the log filter",
        .description = "Change which log classes/levels are emitted, using the emulator's filter "
                       "syntax: space-separated '<Class[.Sub]>:<Level>' rules applied left to "
                       "right, e.g. '*:Info Service.NVDRV:Debug Render.Vulkan:Warning'. Levels: "
                       "Trace, Debug, Info, Warning, Error, Critical. Returns the previous filter.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"filter", {{"type", "string"}, {"minLength", 1}}}}},
                         {"required", {"filter"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = false, .idempotent = true},
        .requires_control = true,
        .handler = SetLogFilter,
    });
}

void RegisterEmulationTools(ToolRegistry& registry);
void RegisterInputTools(ToolRegistry& registry);
void RegisterDebugTools(ToolRegistry& registry);
void RegisterControlTools(ToolRegistry& registry);
void RegisterActTools(ToolRegistry& registry);
void RegisterTasTools(ToolRegistry& registry);
void RegisterPerfTools(ToolRegistry& registry);
void RegisterProcessTools(ToolRegistry& registry);
void RegisterMemoryTools(ToolRegistry& registry);
void RegisterTraceTools(ToolRegistry& registry);
void RegisterSettingsTools(ToolRegistry& registry);
void RegisterGameStateTools(ToolRegistry& registry);
void RegisterTimelineTools(ToolRegistry& registry);
void RegisterDebuggerTools(ToolRegistry& registry);
void RegisterArtifactTools(ToolRegistry& registry);
void RegisterAbTestTools(ToolRegistry& registry);

void RegisterAllTools(ToolRegistry& registry) {
    RegisterTimelineTools(registry);
    RegisterGameStateTools(registry);
    RegisterSettingsTools(registry);
    RegisterTraceTools(registry);
    RegisterTasTools(registry);
    RegisterPerfTools(registry);
    RegisterProcessTools(registry);
    RegisterMemoryTools(registry);
    RegisterCoreTools(registry);
    RegisterEmulationTools(registry);
    RegisterInputTools(registry);
    RegisterDebugTools(registry);
    RegisterControlTools(registry);
    RegisterActTools(registry);
    RegisterDebuggerTools(registry);
    RegisterArtifactTools(registry);
    RegisterAbTestTools(registry);
}

} // namespace Mcp
