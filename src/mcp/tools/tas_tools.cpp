// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frame-locked scripted input through the built-in TAS engine (TAS-nx scripts, advanced once per
// presented frame), plus recording what a human plays so an agent can replay it.

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <thread>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

using InputCommon::TasInput::Tas;
using InputCommon::TasInput::TasState;

/// Largest script tas_write_script will write.
constexpr size_t kMaxScriptBytes = 4 * 1024 * 1024;

/// Button names shared with the input tools; HOME and CAPTURE are left out (the TAS engine
/// ignores them so scripts can't trigger hotkeys).
constexpr std::array<std::string_view, 18> kTasButtons{
    "A",     "B",    "X",   "Y",     "LSTICK", "RSTICK", "L",  "R",  "ZL",
    "ZR",    "PLUS", "MINUS", "DLEFT", "DUP",  "DRIGHT", "DDOWN", "SL", "SR"};

/// The TAS button names as a JSON array, for schema enums.
json TasButtonEnum() {
    json names = json::array();
    for (const auto name : kTasButtons) {
        names.push_back(name);
    }
    return names;
}

/// Path the TAS engine loads player N's script from (TAS dir/script0-N.txt).
std::filesystem::path ScriptPath(int player) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir) /
           fmt::format("script0-{}.txt", player);
}

std::filesystem::path RecordPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir) / "record.txt";
}

/// Lowercase name of a TAS engine state.
const char* StateName(TasState state) {
    switch (state) {
    case TasState::Running:
        return "running";
    case TasState::Recording:
        return "recording";
    case TasState::Stopped:
        return "stopped";
    }
    return "?";
}

/// Returns the frontend's TAS driver, or nullptr if there is no host or input subsystem.
Tas* TasEngine(ToolContext& ctx) {
    if (!ctx.host) {
        return nullptr;
    }
    auto* input = ctx.host->GetInputSubsystem();
    return input ? input->GetTas() : nullptr;
}

/// Reads the engine's status. Called from the MCP thread: the values are plain integers the
/// render thread updates once per frame (the Qt status bar reads them the same way).
json StatusJson(Tas& tas) {
    const auto [state, frame, lengths] = tas.GetStatus();
    json script_lengths = json::array();
    for (const size_t length : lengths) {
        script_lengths.push_back(length);
    }
    return json{{"state", StateName(state)},
                {"frame", frame},
                {"script_lengths", std::move(script_lengths)},
                {"tas_enabled", Settings::values.tas_enable.GetValue()},
                {"loop", Settings::values.tas_loop.GetValue()},
                {"scripts_dir",
                 Common::FS::PathToUTF8String(
                     Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir))}};
}

/// Formats an optional [x, y] stick in -1..1 as a TAS-nx "x;y" axis pair in +-32767 ("0;0" if
/// null).
std::string Axis(const json& stick) {
    if (stick.is_null()) {
        return "0;0";
    }
    return fmt::format("{};{}", static_cast<int>(stick[0].get<double>() * 32767.0),
                       static_cast<int>(stick[1].get<double>() * 32767.0));
}

/// Expands [{frames, buttons?, left?, right?}] into TAS-nx lines (one per non-neutral frame,
/// plus the last frame so the script has its full length).
std::string ExpandSteps(const json& steps, size_t& total_frames) {
    std::string out;
    size_t frame = 0;
    for (const auto& step : steps) {
        const size_t frames = step.value("frames", size_t{1});
        std::string keys;
        for (const auto& b : step.value("buttons", json::array())) {
            keys += (keys.empty() ? "KEY_" : ";KEY_") + b.get<std::string>();
        }
        if (keys.empty()) {
            keys = "NONE";
        }
        const std::string left = Axis(step.value("left", json{}));
        const std::string right = Axis(step.value("right", json{}));
        const bool neutral = keys == "NONE" && left == "0;0" && right == "0;0";
        for (size_t i = 0; i < frames; ++i, ++frame) {
            const bool last = &step == &steps.back() && i + 1 == frames;
            if (!neutral || last) {
                out += fmt::format("{} {} {} {}\n", frame, keys, left, right);
            }
        }
    }
    total_frames = frame;
    return out;
}

/// tas_status: the engine's StatusJson, or an error if the frontend has no TAS engine.
ToolResult TasStatus(ToolContext& ctx, const json&) {
    Tas* tas = TasEngine(ctx);
    if (!tas) {
        return ToolResult::Error("this frontend has no TAS engine");
    }
    return ToolResult::Structured(StatusJson(*tas));
}

/// tas_write_script: writes player N's script from 'steps' (expanded) or a raw 'script', then
/// enables TAS and reloads the scripts through the host. Errors if neither argument is given,
/// the text exceeds kMaxScriptBytes, or the file cannot be written.
ToolResult TasWriteScript(ToolContext& ctx, const json& args) {
    const int player = args.value("player", 1);
    std::string text;
    size_t frames = 0;
    if (args.contains("steps")) {
        text = ExpandSteps(args["steps"], frames);
    } else if (args.contains("script")) {
        text = args["script"].get<std::string>();
        if (!text.empty() && text.back() != '\n') {
            text += '\n';
        }
    } else {
        return ToolResult::Error("pass either 'steps' or 'script'");
    }
    if (text.size() > kMaxScriptBytes) {
        return ToolResult::Error("script larger than 4 MiB");
    }
    const auto path = ScriptPath(player);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile, text) != text.size()) {
        return ToolResult::Error("could not write " + Common::FS::PathToUTF8String(path));
    }
    // Reload the scripts (applied on the next presented frame; also works with no game running).
    if (ctx.host) {
        ctx.host->WithTas(false, [](Tas& tas) {
            Settings::values.tas_enable.SetValue(true);
            tas.Reset();
        });
    }
    json out{{"path", Common::FS::PathToUTF8String(path)}, {"bytes", text.size()}};
    if (frames != 0) {
        out["frames"] = frames;
    }
    return ToolResult::Structured(std::move(out));
}

/// tas_read_script: returns a player's script file or the last recording, cut to max_bytes
/// (default 64 KiB). A missing file reads as an empty script.
ToolResult TasReadScript(ToolContext&, const json& args) {
    const std::string which = args.value("which", "1");
    std::filesystem::path path;
    if (which == "record") {
        path = RecordPath();
    } else {
        const int player = std::atoi(which.c_str());
        if (player < 1 || player > 8) {
            return ToolResult::Error("which: a player number 1-8 or \"record\"");
        }
        path = ScriptPath(player);
    }
    std::string text =
        Common::FS::ReadStringFromFile(path, Common::FS::FileType::TextFile);
    const size_t limit = args.value("max_bytes", size_t{65536});
    json out{{"path", Common::FS::PathToUTF8String(path)}, {"bytes", text.size()}};
    if (text.size() > limit) {
        text.resize(limit);
        out["truncated"] = true;
    }
    out["script"] = std::move(text);
    return ToolResult::Structured(std::move(out));
}

/**
 * tas_play: restarts the loaded scripts from frame 0. With wait (the default) it polls the engine
 * every 2 ms until the script stops or wraps, timeout_ms passes or the server stops, taking a
 * screenshot when the script frame reaches each screenshot_at entry.
 * @return Completion, frames played, the screenshots, and warning-or-worse timeline events
 *         pushed since the call started.
 */
ToolResult TasPlay(ToolContext& ctx, const json& args) {
    using namespace std::chrono;
    Tas* tas = TasEngine(ctx);
    if (!tas) {
        return ToolResult::Error("this frontend has no TAS engine");
    }
    const u64 since_seq = ctx.timeline ? ctx.timeline->LastSeq() : 0;
    // Stop, reload from frame 0, start: the reload happens on the next frame, before playing.
    std::string error = ctx.host->WithTas(true, [](Tas& t) {
        Settings::values.tas_enable.SetValue(true);
        t.Stop();
        t.Reset();
        t.StartStop();
    });
    if (!error.empty()) {
        return ToolResult::Error("tas_play: " + error);
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "mcp", "tas play");
    }
    if (!args.value("wait", true)) {
        return ToolResult::Structured(json{{"started", true}, {"since_seq", since_seq}});
    }

    std::vector<u64> checkpoints;
    for (const auto& f : args.value("screenshot_at", json::array())) {
        checkpoints.push_back(f.get<u64>());
    }
    std::sort(checkpoints.begin(), checkpoints.end());
    const u32 max_width = args.value("max_width", 640U);
    const auto deadline = steady_clock::now() + milliseconds{args.value("timeout_ms", 120000)};
    const auto started = steady_clock::now();

    ToolResult images;
    json shots = json::array();
    size_t next_checkpoint = 0;
    bool seen_running = false;
    bool completed = false;
    size_t last_frame = 0;
    size_t length = 0;
    while (steady_clock::now() < deadline && !ServerStopping()) {
        const auto [state, frame, lengths] = tas->GetStatus();
        length = *std::max_element(lengths.begin(), lengths.end());
        if (state == TasState::Running) {
            seen_running = true;
            last_frame = frame;
        }
        while (next_checkpoint < checkpoints.size() && seen_running &&
               frame >= checkpoints[next_checkpoint]) {
            std::string shot_error;
            if (const auto png = ctx.host->CaptureScreenshotPng(max_width, milliseconds{3000},
                                                                shot_error)) {
                images.AddImage(*png, "image/png");
                shots.push_back({{"requested_frame", checkpoints[next_checkpoint]},
                                 {"script_frame", std::get<1>(tas->GetStatus())}});
            } else {
                shots.push_back({{"requested_frame", checkpoints[next_checkpoint]},
                                 {"error", shot_error}});
            }
            ++next_checkpoint;
        }
        // Done once it ran and is stopped again (or wrapped, when looping).
        if (seen_running && (state == TasState::Stopped || frame < last_frame)) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(milliseconds{2});
    }
    json out{{"completed", completed},
             {"script_length", length},
             {"frames_played", completed ? length : last_frame},
             {"elapsed_ms", duration_cast<milliseconds>(steady_clock::now() - started).count()},
             {"screenshots", std::move(shots)}};
    if (!seen_running) {
        out["hint"] = "TAS never started: is a script loaded (tas_status script_lengths)?";
    } else if (!completed) {
        out["hint"] = "Timed out; the script is still playing (tas_status / tas_stop).";
    }
    if (ctx.timeline) {
        Timeline::Query q;
        q.since_seq = since_seq;
        q.min_level = 3; // warning
        q.limit = 40;
        json events = json::array();
        for (const auto& e : ctx.timeline->Read(q).events) {
            events.push_back(EventJson(e));
        }
        out["timeline"] = std::move(events);
    }
    ToolResult result = ToolResult::Structured(std::move(out));
    for (auto& block : images.content) {
        result.content.push_back(std::move(block));
    }
    return result;
}

/// tas_stop: stops script playback (recording is left alone) and returns the new status.
ToolResult TasStop(ToolContext& ctx, const json&) {
    if (std::string error = ctx.host->WithTas(false, [](Tas& t) { t.Stop(); }); !error.empty()) {
        return ToolResult::Error(error);
    }
    Tas* tas = TasEngine(ctx);
    return ToolResult::Structured(tas ? StatusJson(*tas) : json{{"stopped", true}});
}

/// tas_record_start: enables TAS and starts recording, or reports success if already recording.
/// Needs a running game.
ToolResult TasRecordStart(ToolContext& ctx, const json&) {
    bool recording = false;
    std::string error = ctx.host->WithTas(true, [&](Tas& t) {
        Settings::values.tas_enable.SetValue(true);
        if (std::get<0>(t.GetStatus()) != TasState::Recording) {
            recording = t.Record();
        } else {
            recording = true;
        }
    });
    if (!error.empty()) {
        return ToolResult::Error(error);
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "mcp", "tas record start");
    }
    return ToolResult::Structured(
        json{{"recording", recording},
             {"note", "Records player 1 (or handheld) once per frame, whatever the input source. "
                      "Call tas_record_stop to save."}});
}

/// tas_record_stop: ends recording and saves it to record.txt, and also over player 1's script
/// when save_as_script is set. Errors if nothing was recording.
ToolResult TasRecordStop(ToolContext& ctx, const json& args) {
    const bool overwrite = args.value("save_as_script", false);
    size_t frames = 0;
    bool was_recording = false;
    std::string error = ctx.host->WithTas(false, [&](Tas& t) {
        const auto [state, frame, lengths] = t.GetStatus();
        if (state != TasState::Recording) {
            return;
        }
        was_recording = true;
        frames = frame;
        t.Record(); // toggles off
        t.SaveRecording(overwrite);
    });
    if (!error.empty()) {
        return ToolResult::Error(error);
    }
    if (!was_recording) {
        return ToolResult::Error("not recording (call tas_record_start first)");
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "mcp", fmt::format("tas record stop ({} frames)", frames));
    }
    json out{{"frames", frames}, {"record_path", Common::FS::PathToUTF8String(RecordPath())}};
    if (overwrite) {
        out["script_path"] = Common::FS::PathToUTF8String(ScriptPath(1));
    }
    return ToolResult::Structured(std::move(out));
}

/// tas_set_options: sets the tas_loop and tas_enable settings directly (no host call) and
/// returns the previous and new values.
ToolResult TasSetOptions(ToolContext&, const json& args) {
    json previous{{"loop", Settings::values.tas_loop.GetValue()},
                  {"tas_enabled", Settings::values.tas_enable.GetValue()}};
    if (args.contains("loop")) {
        Settings::values.tas_loop.SetValue(args["loop"].get<bool>());
    }
    if (args.contains("enabled")) {
        Settings::values.tas_enable.SetValue(args["enabled"].get<bool>());
    }
    return ToolResult::Structured(json{{"previous", previous},
                                       {"loop", Settings::values.tas_loop.GetValue()},
                                       {"tas_enabled", Settings::values.tas_enable.GetValue()}});
}

json PlayerProp() {
    return json{{"type", "integer"}, {"minimum", 1}, {"maximum", 8},
                {"description", "Player 1-8 (default 1). Player 1's script also drives handheld."}};
}

json StickProp() {
    return json{{"type", "array"},
                {"items", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "[x, y] in -1..1 (up is +y)."}};
}

} // namespace

/// Registers the TAS script, playback, recording and status tools.
void RegisterTasTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "tas_status",
        .title = "TAS status",
        .description = "TAS engine state (stopped/running/recording), current script frame, the "
                       "loaded script length per player, and options.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = TasStatus,
    });

    registry.Add(Tool{
        .name = "tas_write_script",
        .title = "Write a TAS script",
        .description =
            "Write player N's TAS script and reload it. TAS input is frame-locked (one script line "
            "per presented guest frame), so it is reliable when frame times swing, unlike the "
            "wall-clock input tools. Give either 'steps' ([{frames, buttons?, left?, right?}], "
            "expanded for you; e.g. [{frames:30}, {frames:4, buttons:[\"A\"]}, {frames:60}]) or a "
            "raw TAS-nx 'script' ('<frame> KEY_A;KEY_B <lx;ly> <rx;ry>' per line, axes "
            "+-32767, NONE for no buttons). Then call tas_play.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"player", PlayerProp()},
               {"steps",
                {{"type", "array"},
                 {"minItems", 1},
                 {"maxItems", 5000},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 216000}}},
                     {"buttons", {{"type", "array"},
                                  {"items", {{"type", "string"}, {"enum", TasButtonEnum()}}}}},
                     {"left", StickProp()},
                     {"right", StickProp()}}},
                   {"required", {"frames"}},
                   {"additionalProperties", false}}}}},
               {"script", {{"type", "string"}, {"maxLength", 4194304}}}}},
             {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = TasWriteScript,
    });

    registry.Add(Tool{
        .name = "tas_read_script",
        .title = "Read a TAS script",
        .description = "Read player N's script, or \"record\" for the last recording.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"which", {{"type", "string"},
                                      {"description", "\"1\"-\"8\" (default \"1\") or \"record\"."}}},
                           {"max_bytes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1048576}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = TasReadScript,
    });

    registry.Add(Tool{
        .name = "tas_play",
        .title = "Play the TAS scripts",
        .description =
            "Play the loaded scripts from frame 0 (all players at once). By default waits until "
            "they finish (or timeout_ms) and returns frames played, warning+ timeline events, and "
            "screenshots taken when the script reaches each frame in screenshot_at (approximate "
            "by a frame or two: the game keeps running while capturing).",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"wait", {{"type", "boolean"}, {"description", "Wait for the end (default true)."}}},
               {"timeout_ms", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}},
               {"screenshot_at",
                {{"type", "array"},
                 {"maxItems", 12},
                 {"items", {{"type", "integer"}, {"minimum", 0}}},
                 {"description", "Script frames to screenshot at."}}},
               {"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 3840}}}}},
             {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = TasPlay,
    });

    registry.Add(Tool{
        .name = "tas_stop",
        .title = "Stop TAS playback",
        .description = "Stop TAS playback (input returns to the other sources).",
        .annotations = {.idempotent = true},
        .requires_write = true,
        .requires_control = true,
        .handler = TasStop,
    });

    registry.Add(Tool{
        .name = "tas_record_start",
        .title = "Start recording input",
        .description = "Start recording player 1's input every frame (a human playing, or the "
                       "input tools). Stop with tas_record_stop.",
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = TasRecordStart,
    });

    registry.Add(Tool{
        .name = "tas_record_stop",
        .title = "Stop recording and save",
        .description = "Stop recording and save it as record.txt; with save_as_script also as "
                       "player 1's script, ready for tas_play.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"save_as_script", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = TasRecordStop,
    });

    registry.Add(Tool{
        .name = "tas_set_options",
        .title = "TAS options",
        .description = "Set TAS options: loop (replay forever), enabled (the TAS engine; the "
                       "other TAS tools turn it on themselves). Returns the previous values.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"loop", {{"type", "boolean"}}}, {"enabled", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = TasSetOptions,
    });
}

} // namespace Mcp
