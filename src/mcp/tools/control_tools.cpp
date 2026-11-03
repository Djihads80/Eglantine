// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Sharing one emulator between several agents: the control lease, and closing the emulator.

#include <fmt/format.h>

#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"

namespace Mcp {

namespace {

/// Describes a lease holder for the caller: whether it is them, a short session id, and how long
/// the lease has been held and has left, in whole seconds.
json HolderJson(const ControlLease::Holder& h, const std::string& caller) {
    const auto now = ControlLease::Clock::now();
    return json{
        {"held_by_you", h.session_id == caller},
        {"label", h.label},
        {"session", h.session_id.substr(0, 8)},
        {"held_for_s",
         std::chrono::duration_cast<std::chrono::seconds>(now - h.acquired).count()},
        {"expires_in_s",
         std::chrono::duration_cast<std::chrono::seconds>(h.expires - now).count()},
    };
}

/// control_status: reports whether the lease is free, or who holds it.
ToolResult ControlStatus(ToolContext& ctx, const json&) {
    if (!ctx.control) {
        return ToolResult::Error("control leases are not available");
    }
    const auto holder = ctx.control->Current();
    if (!holder) {
        return ToolResult::Structured(json{
            {"free", true},
            {"note", "Nobody controls the emulator; the first state-changing tool call takes it."}});
    }
    json out = HolderJson(*holder, ctx.session_id);
    out["free"] = false;
    return ToolResult::Structured(std::move(out));
}

/// acquire_control: takes or renews the lease for the calling session. If another session holds
/// it, returns that holder as an error result with "acquired": false. Logs success to the timeline.
ToolResult AcquireControl(ToolContext& ctx, const json& args) {
    if (!ctx.control) {
        return ToolResult::Error("control leases are not available");
    }
    std::optional<std::chrono::seconds> ttl;
    if (args.contains("ttl_s")) {
        ttl = std::chrono::seconds{args["ttl_s"].get<int>()};
    }
    ControlLease::Holder holder;
    const std::string label = args.value("label", "");
    if (!ctx.control->Acquire(ctx.session_id, label, ttl, holder)) {
        json out = HolderJson(holder, ctx.session_id);
        out["acquired"] = false;
        ToolResult result = ToolResult::Structured(std::move(out));
        result.is_error = true;
        return result;
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp",
                           fmt::format("control acquired by {}", label.empty() ? ctx.session_id.substr(0, 8) : label));
    }
    json out = HolderJson(holder, ctx.session_id);
    out["acquired"] = true;
    return ToolResult::Structured(std::move(out));
}

/// release_control: frees the lease if the caller holds it; "released" says whether it did.
ToolResult ReleaseControl(ToolContext& ctx, const json&) {
    if (!ctx.control) {
        return ToolResult::Error("control leases are not available");
    }
    const bool released = ctx.control->Release(ctx.session_id);
    if (released && ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", "control released");
    }
    return ToolResult::Structured(json{{"released", released}});
}

/// quit_emulator: asks the frontend to close. The close is asynchronous, so the reply is sent
/// before the window (and this server) goes away.
ToolResult QuitEmulator(ToolContext& ctx, const json&) {
    if (!ctx.host) {
        return ToolResult::Error("no frontend host");
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", "quit requested");
    }
    if (const std::string error = ctx.host->Quit(); !error.empty()) {
        return ToolResult::Error("quit failed: " + error);
    }
    return ToolResult::Text("Stopping emulation (if running) and closing the emulator. The MCP "
                            "server goes away with it.");
}

} // namespace

/// Registers the control lease tools and quit_emulator.
void RegisterControlTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "control_status",
        .title = "Who controls the emulator",
        .description = "Whether another agent currently holds the control lease. State-changing "
                       "tools (boot, pause, stop, input, quit...) need it; read-only tools never do.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = ControlStatus,
    });
    registry.Add(Tool{
        .name = "acquire_control",
        .title = "Take control of the emulator",
        .description = "Take the control lease so no other agent changes emulator state while you "
                       "work. Not needed for a single agent: the first state-changing call takes it "
                       "automatically. Every controlled call renews it; it expires after ttl_s "
                       "seconds of inactivity (default 120). Fails if another agent holds it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"label", {{"type", "string"}, {"maxLength", 80},
                                      {"description", "Your name, shown to other agents."}}},
                           {"ttl_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 3600}}}}},
                         {"additionalProperties", false}},
        .annotations = {.idempotent = true},
        .handler = AcquireControl,
    });
    registry.Add(Tool{
        .name = "release_control",
        .title = "Release control",
        .description = "Give up the control lease so other agents can drive the emulator.",
        .annotations = {.idempotent = true},
        .handler = ReleaseControl,
    });
    registry.Add(Tool{
        .name = "quit_emulator",
        .title = "Close the emulator",
        .description = "Stop the running game (without the confirmation prompt) and close the "
                       "emulator. Use before rebuilding, since the executable can't be replaced "
                       "while it runs. The MCP server shuts down with it.",
        .annotations = {.destructive = true},
        .requires_control = true,
        .handler = QuitEmulator,
    });
}

} // namespace Mcp
