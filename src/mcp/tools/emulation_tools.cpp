// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Lifecycle and screen tools, implemented through the frontend Host.

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/loader/loader.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// ASCII lower-case copy of s, for case-insensitive matching.
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

ToolResult HostMissing() {
    return ToolResult::Error("no frontend host is attached");
}

/// list_games: the host's game list, filtered by a case-insensitive substring of title or path.
ToolResult ListGames(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const auto games = ctx.host->ListGames();
    if (!games) {
        return ToolResult::Error("this frontend has no game list");
    }
    const std::string filter = Lower(args.value("filter", ""));
    json list = json::array();
    for (const auto& g : *games) {
        if (!filter.empty() && Lower(g.title).find(filter) == std::string::npos &&
            Lower(g.path).find(filter) == std::string::npos) {
            continue;
        }
        list.push_back({{"title", g.title},
                        {"program_id", fmt::format("{:016X}", g.program_id)},
                        {"path", g.path}});
    }
    return ToolResult::Structured(json{{"games", std::move(list)}});
}

/// boot_game: boots 'path', or else the single game list entry whose program id equals
/// 'program_id' or whose title contains 'title'. Errors on no match or several matches. Returns
/// the timeline sequence from before the boot so the caller can follow it.
ToolResult BootGame(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    std::string path = args.value("path", "");
    if (path.empty()) {
        const auto games = ctx.host->ListGames();
        if (!games) {
            return ToolResult::Error("pass 'path': this frontend has no game list");
        }
        const std::string query = Lower(args.value("title", ""));
        const std::string program_id = Lower(args.value("program_id", ""));
        std::vector<const GameEntry*> matches;
        for (const auto& g : *games) {
            const bool id_match =
                !program_id.empty() && Lower(fmt::format("{:016x}", g.program_id)) == program_id;
            const bool title_match = !query.empty() && Lower(g.title).find(query) != std::string::npos;
            if (id_match || title_match) {
                matches.push_back(&g);
            }
        }
        if (matches.empty()) {
            return ToolResult::Error("no game in the list matches; call list_games");
        }
        if (matches.size() > 1) {
            std::string names;
            for (const auto* g : matches) {
                names += fmt::format("\n- {} ({})", g->title, g->path);
            }
            return ToolResult::Error("several games match, pass 'path' or a more specific title:" +
                                     names);
        }
        path = matches.front()->path;
    }

    const u64 since = ctx.timeline ? ctx.timeline->LastSeq() : 0;
    if (const std::string error = ctx.host->BootGame(path); !error.empty()) {
        return ToolResult::Error("boot failed: " + error);
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", "boot requested: " + path);
    }
    json out{{"booted", path},
             {"since_seq", since},
             {"hint", "Use wait_for or timeline with since_seq to follow the boot; screenshot once "
                      "frames are presented."}};
    // With the GDB stub on, the game waits for a debugger before it runs: attach so it starts.
    if (Settings::values.use_gdbstub.GetValue()) {
        const std::string error = AttachDebuggerAfterBoot(ctx);
        out["debugger"] = error.empty() ? "attached (the GDB stub is on; breakpoint_add works)"
                                        : "not attached: " + error;
    }
    return ToolResult::Structured(std::move(out));
}

/// Shared handler for pause/resume/stop: calls the given Host action and logs a State event on
/// success. `what` names the action in the reply and the timeline.
ToolResult Lifecycle(ToolContext& ctx, std::string (Host::*action)(), std::string_view what) {
    if (!ctx.host) {
        return HostMissing();
    }
    if (const std::string error = (ctx.host->*action)(); !error.empty()) {
        return ToolResult::Error(fmt::format("{} failed: {}", what, error));
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", std::string{what});
    }
    return ToolResult::Text(fmt::format("{}: ok", what));
}

/// screenshot: PNG of the next presented frame plus the guest present count at reply time.
ToolResult Screenshot(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u32 max_width = args.value("max_width", 1280U);
    const auto timeout = std::chrono::milliseconds{args.value("timeout_ms", 5000)};
    std::string error;
    const auto png = ctx.host->CaptureScreenshotPng(max_width, timeout, error);
    if (!png) {
        return ToolResult::Error("screenshot failed: " + error);
    }
    ToolResult result;
    result.AddImage(*png, "image/png");
    result.AddText(fmt::format("guest frame {}",
                               Core::g_nx2_present_count.load(std::memory_order_relaxed)));
    return result;
}

/// set_docked: switches console mode through the host and logs it on the timeline.
ToolResult SetDocked(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const bool docked = args.at("docked").get<bool>();
    if (std::string error = ctx.host->SetDocked(docked); !error.empty()) {
        return ToolResult::Error("set_docked failed: " + error);
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", docked ? "docked" : "handheld");
    }
    return ToolResult::Text(docked ? "docked mode" : "handheld mode");
}

/// Runs N frames then pauses again (resume, count presented frames, pause). The pause lands on
/// the frame after the Nth presentation at best; expect +/- a frame or two.
ToolResult FrameAdvance(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u64 frames = args.value("frames", u64{1});
    if (!ctx.system->IsPaused()) {
        if (std::string error = ctx.host->Pause(); !error.empty()) {
            return ToolResult::Error("pause failed: " + error);
        }
    }
    const u64 start = PresentedFrames();
    if (std::string error = ctx.host->Resume(); !error.empty()) {
        return ToolResult::Error("resume failed: " + error);
    }
    const u64 got = WaitFrames(frames, std::chrono::milliseconds{args.value("timeout_ms", 10000)});
    ctx.host->Pause();
    ToolResult result = ToolResult::Structured(
        json{{"requested", frames}, {"advanced", PresentedFrames() - start}, {"counted", got},
             {"paused", true}});
    if (args.value("screenshot", true)) {
        std::string error;
        // Paused: the renderer only fills screenshots on present, so use the capture layer.
        if (const auto png = RawCapturePng(ctx, 640, error)) {
            result.AddImage(*png, "image/png");
        } else {
            result.AddText("no screenshot: " + error);
        }
    }
    return result;
}

/// capture_frames: takes `count` screenshots, waiting every_frames presents (up to 20 s) between
/// them. A failed capture is reported in its slot and does not stop the burst.
ToolResult CaptureFrames(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u64 every = args.value("every_frames", u64{30});
    const int count = args.value("count", 4);
    const u32 width = args.value("max_width", 480U);
    ToolResult images;
    json frames = json::array();
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            WaitFrames(every, std::chrono::milliseconds{20000});
        }
        std::string error;
        if (const auto png = ctx.host->CaptureScreenshotPng(width, std::chrono::milliseconds{3000},
                                                            error)) {
            images.AddImage(*png, "image/png");
            frames.push_back(PresentedFrames());
        } else {
            frames.push_back(json{{"error", error}});
        }
    }
    ToolResult result = ToolResult::Structured(json{{"frames", std::move(frames)}});
    for (auto& block : images.content) {
        result.content.push_back(std::move(block));
    }
    return result;
}

/// game_info: running game metadata from the app loader. Fields the loader cannot read are left
/// out; the icon is attached as JPEG unless icon is false.
ToolResult GameInfo(ToolContext& ctx, const json& args) {
    auto& loader = ctx.system->GetAppLoader();
    json out{{"program_id", fmt::format("{:016X}", ctx.system->GetApplicationProcessProgramID())}};
    std::string text;
    if (loader.ReadTitle(text) == Loader::ResultStatus::Success) {
        out["title"] = text;
    }
    if (loader.ReadDisplayVersion(text) == Loader::ResultStatus::Success) {
        out["display_version"] = text;
    }
    FileSys::NACP nacp;
    if (loader.ReadControlData(nacp) == Loader::ResultStatus::Success) {
        out["developer"] = nacp.GetDeveloperName();
    }
    out["build_id"] = [&] {
        std::string hex;
        for (const u8 b : ctx.system->GetApplicationProcessBuildID()) {
            hex += fmt::format("{:02x}", b);
        }
        return hex;
    }();
    out["file_type"] = Loader::GetFileTypeString(loader.GetFileType());
    ToolResult result = ToolResult::Structured(std::move(out));
    std::vector<u8> icon;
    if (args.value("icon", true) && loader.ReadIcon(icon) == Loader::ResultStatus::Success &&
        !icon.empty()) {
        result.AddImage(icon, "image/jpeg");
    }
    return result;
}

} // namespace

/// Registers the boot, pause/resume/stop, docked mode, frame and screenshot tools.
void RegisterEmulationTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "set_docked",
        .title = "Docked / handheld",
        .description = "Switch between docked and handheld mode live, as the emulator's toggle "
                       "does (player 1 becomes a Pro Controller when docking from handheld).",
        .input_schema = {{"type", "object"},
                         {"properties", {{"docked", {{"type", "boolean"}}}}},
                         {"required", {"docked"}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = SetDocked,
    });
    registry.Add(Tool{
        .name = "frame_advance",
        .title = "Advance frames",
        .description = "Pause (if needed), run N presented frames, pause again, and return a "
                       "screenshot. Good for stepping through an effect or a glitch; accuracy is "
                       "about a frame (pausing isn't frame-locked).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}}},
                           {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}}},
                           {"screenshot", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_control = true,
        .handler = FrameAdvance,
    });
    registry.Add(Tool{
        .name = "capture_frames",
        .title = "Capture a frame burst",
        .description = "Take count screenshots (max 12), one every every_frames presented frames: "
                       "a cheap substitute for video when watching an animation or a glitch.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"every_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
                           {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 12}}},
                           {"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 1280}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = CaptureFrames,
    });
    registry.Add(Tool{
        .name = "game_info",
        .title = "Running game info",
        .description = "Metadata of the running game: title, display version, developer, program "
                       "id, build id, file type, and its icon (unless icon:false).",
        .input_schema = {{"type", "object"},
                         {"properties", {{"icon", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = GameInfo,
    });
    registry.Add(Tool{
        .name = "list_games",
        .title = "List games",
        .description = "Games in the emulator's game list (title, program id, path). Optional "
                       "case-insensitive filter on title or path.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"filter", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = ListGames,
    });
    registry.Add(Tool{
        .name = "boot_game",
        .title = "Boot a game",
        .description = "Boot a game by file path, by program id, or by (unique) title match from "
                       "the game list. Fails if a game is already running (call stop first). "
                       "Returns since_seq: follow the boot with timeline/wait_for from there.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"path", {{"type", "string"}}},
                           {"program_id", {{"type", "string"}, {"description", "16 hex digits"}}},
                           {"title", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.destructive = true},
        .requires_control = true,
        .handler = BootGame,
    });
    registry.Add(Tool{
        .name = "pause",
        .title = "Pause emulation",
        .description = "Pause the running game.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_control = true,
        .handler = [](ToolContext& ctx, const json&) { return Lifecycle(ctx, &Host::Pause, "pause"); },
    });
    registry.Add(Tool{
        .name = "resume",
        .title = "Resume emulation",
        .description = "Resume a paused game.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_control = true,
        .handler = [](ToolContext& ctx, const json&) { return Lifecycle(ctx, &Host::Resume, "resume"); },
    });
    registry.Add(Tool{
        .name = "stop",
        .title = "Stop emulation",
        .description = "Stop the running game (no confirmation, unsaved game progress is lost).",
        .annotations = {.destructive = true, .idempotent = true},
        .requires_game = true,
        .requires_control = true,
        .handler = [](ToolContext& ctx, const json&) { return Lifecycle(ctx, &Host::Stop, "stop"); },
    });
    registry.Add(Tool{
        .name = "screenshot",
        .title = "Screenshot",
        .description = "PNG of the next presented frame (as displayed, including post-processing), "
                       "scaled to max_width (16:9). Fails if no frame is presented within "
                       "timeout_ms, e.g. while paused or loading.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 3840}}},
                           {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = Screenshot,
    });
}

} // namespace Mcp
