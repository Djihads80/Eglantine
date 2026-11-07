// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// `act`: input, wait and observe in one call, so a "press A and see what happened" step costs one
// agent turn instead of four or five.

#include <chrono>
#include <thread>

#include <fmt/format.h>

#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// Whether the `capture` array names `what`.
bool Wants(const json& capture, std::string_view what) {
    for (const auto& c : capture) {
        if (c.get<std::string>() == what) {
            return true;
        }
    }
    return false;
}

/// act tool: runs optional input, waits (event, screen, frames, then ms, each with its own
/// timeout), then captures what was asked for. Blocks the calling thread for the whole wait.
/// Input needs writes and the control lease; a timed-out wait is reported, not an error.
ToolResult Act(ToolContext& ctx, const json& args) {
    using namespace std::chrono;
    const auto started = steady_clock::now();
    const bool has_input = args.contains("input");

    // Input changes emulator state: same gates as the input tools, checked here because act is
    // also useful read-only (wait + capture).
    if (has_input) {
        if (!ctx.writes_allowed) {
            return ToolResult::Error(
                "act with 'input' needs MCP writes. Ask the user to enable Tools > MCP Server > "
                "Allow Writes, or relaunch with --mcp-allow-writes. (act without input works.)");
        }
        if (ctx.control) {
            ControlLease::Holder holder;
            if (!ctx.control->Touch(ctx.session_id, holder)) {
                return ToolResult::Error(fmt::format(
                    "Another agent controls the emulator (session {}...). act without input still "
                    "works; call control_status to check.",
                    holder.session_id.substr(0, 8)));
            }
        }
    }

    const u64 since_seq = ctx.timeline ? ctx.timeline->LastSeq() : 0;
    const u64 frame_before = PresentedFrames();
    json out;

    if (has_input) {
        const size_t npad = NpadIndex(args);
        if (std::string error = RunInputSteps(ctx, npad, args["input"]); !error.empty()) {
            return ToolResult::Error("input failed: " + error);
        }
    }

    // Wait: for an event (if given), then for frames, then for wall time.
    const json wait = args.value("wait", json{{"frames", 10}});
    json waited = json::object();
    if (wait.contains("event") && ctx.timeline) {
        Timeline::Query q = ParseQuery(wait["event"]);
        q.since_seq = since_seq;
        const auto timeout = milliseconds{wait["event"].value("timeout_ms", 10000)};
        if (const auto found = ctx.timeline->WaitFor(q, timeout)) {
            waited["event"] = EventJson(*found);
        } else {
            waited["event"] = nullptr;
            waited["event_timed_out"] = true;
        }
    }
    if (wait.contains("screen")) {
        json state;
        const auto timeout = milliseconds{wait.value("screen_timeout_ms", 30000)};
        const std::string hit = GameWaitScreen(ctx, wait["screen"], timeout, state);
        waited["screen"] = hit.empty() ? json() : json(hit);
        if (hit.empty()) {
            waited["screen_timed_out"] = true;
            waited["screen_last"] = state;
        }
    }
    if (wait.contains("frames")) {
        const u64 want = wait["frames"].get<u64>();
        const auto timeout = milliseconds{wait.value("frames_timeout_ms", 20000)};
        const u64 got = WaitFrames(want, timeout);
        waited["frames"] = got;
        if (got < want) {
            waited["frames_timed_out"] = true; // paused, loading or very slow
        }
    }
    if (wait.contains("ms")) {
        SleepUnlessStopping(milliseconds{wait["ms"].get<int>()});
        waited["ms"] = wait["ms"];
    }
    out["waited"] = std::move(waited);

    const json capture =
        args.value("capture", json::array({"screenshot", "timeline", "perf"}));
    ToolResult result;

    if (Wants(capture, "timeline") && ctx.timeline) {
        json filter = args.value("timeline", json::object());
        if (!filter.contains("min_level")) {
            filter["min_level"] = "warning"; // info logs drown everything else
        }
        Timeline::Query q = ParseQuery(filter);
        q.since_seq = since_seq;
        q.limit = filter.value("limit", size_t{60});
        const auto r = ctx.timeline->Read(q);
        json events = json::array();
        for (const auto& e : r.events) {
            events.push_back(EventJson(e));
        }
        out["timeline"] = {{"events", std::move(events)}, {"next_seq", r.next_seq}};
        if (r.truncated) {
            out["timeline"]["truncated"] = true;
        }
        if (r.dropped) {
            out["timeline"]["dropped"] = r.dropped;
        }
    }
    if (Wants(capture, "controller")) {
        out["controller"] = ControllerStateJson(ctx, NpadIndex(args));
    }
    if (Wants(capture, "perf")) {
        out["perf"] = PerfJson(ctx);
    }
    if (Wants(capture, "screen_state")) {
        std::string error;
        json state = GameScreenState(ctx, error);
        out["screen_state"] = state.is_null() ? json{{"error", error}} : std::move(state);
    }
    if (Wants(capture, "probes")) {
        out["probes"] = GameProbes(ctx);
    }
    out["frames"] = {{"before", frame_before}, {"after", PresentedFrames()}};

    if (Wants(capture, "screenshot")) {
        if (!ctx.host) {
            out["screenshot_error"] = "no frontend host";
        } else {
            std::string error;
            const u32 max_width = args.value("max_width", 960U);
            if (const auto png = ctx.host->CaptureScreenshotPng(max_width, milliseconds{5000},
                                                                error)) {
                result.AddImage(*png, "image/png");
            } else {
                out["screenshot_error"] = error;
            }
        }
    }
    out["elapsed_ms"] = duration_cast<milliseconds>(steady_clock::now() - started).count();

    // Structured data first (as text too), then the image.
    ToolResult structured = ToolResult::Structured(std::move(out));
    for (auto& block : result.content) {
        structured.content.push_back(std::move(block));
    }
    return structured;
}

} // namespace

/// Registers act, which runs input steps and reports what happened.
void RegisterActTools(ToolRegistry& registry) {
    json event_filter = FilterProperties();
    event_filter.erase("since_seq");
    event_filter["timeout_ms"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000},
                                  {"description", "How long to wait for it (default 10000)."}};

    json timeline_filter = FilterProperties();
    timeline_filter.erase("since_seq");
    timeline_filter["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 500},
                                {"description", "Max events (default 60)."}};

    registry.Add(Tool{
        .name = "act",
        .title = "Act and observe",
        .description =
            "One call for 'do something and show me what happened': runs optional input steps "
            "(same format as input_sequence), waits (for a timeline event, then N guest frames, "
            "then ms; default 10 frames), and returns what you ask for in capture: a screenshot, "
            "the timeline events since the call started (logs at warning+ by default), controller "
            "state, perf. Without input it needs no writes or control lease, so it doubles as "
            "'wait and look'.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"input", InputStepsSchema()},
               {"player", PlayerSchema()},
               {"wait",
                {{"type", "object"},
                 {"properties",
                  {{"event",
                    {{"type", "object"},
                     {"properties", event_filter},
                     {"additionalProperties", false},
                     {"description", "Wait for the first timeline event matching these filters."}}},
                   {"frames", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3600},
                               {"description", "Guest frames to wait (frame-locked, not wall time)."}}},
                   {"frames_timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 120000}}},
                   {"screen", {{"type", "array"}, {"items", {{"type", "string"}}},
                               {"description", "Wait until screen_state is one of these labels."}}},
                   {"screen_timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600000}}},
                   {"ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}}}}},
                 {"additionalProperties", false}}},
               {"capture",
                {{"type", "array"},
                 {"items",
                  {{"type", "string"},
                   {"enum", {"screenshot", "timeline", "controller", "perf", "screen_state",
                             "probes"}}}},
                 {"description", "Default [screenshot, timeline, perf]."}}},
               {"timeline",
                {{"type", "object"},
                 {"properties", timeline_filter},
                 {"additionalProperties", false},
                 {"description", "Filters for the captured timeline events."}}},
               {"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 3840},
                              {"description", "Screenshot width cap (default 960)."}}}}},
             {"additionalProperties", false}},
        .requires_game = true,
        .handler = Act,
    });
}

} // namespace Mcp
