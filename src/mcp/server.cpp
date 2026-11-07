// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "core/core.h"
#include "mcp/host.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/dashboard.h"
#include "mcp/protocol.h"
#include "mcp/server.h"
#include "mcp/timeline.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

std::atomic<Timeline*> g_active_timeline{nullptr};

// Never destroyed: the log sink, trace hooks and tool threads may have loaded the pointer just
// before a server stopped, and must still be able to push into it.
Timeline& ProcessTimeline() {
    static Timeline* const instance = new Timeline{
        65536, [] { return Core::g_nx2_present_count.load(std::memory_order_relaxed); }};
    return *instance;
}

constexpr const char* kInstructions =
    "Eglantine is a Nintendo Switch 2 emulator. Start with `status`. Driving: `act` does input + "
    "wait + observe in one call; TAS tools give frame-exact input; per-game `route_list` / "
    "`route_run` replay saved recipes (e.g. a game's 'to_gameplay'), and `screen_state` names the "
    "current screen from labelled fingerprints, so prefer those over screenshots. Observing: "
    "`timeline` (keep next_seq, pass it back as since_seq), `wait_for`, `summarize_since`; "
    "unimplemented commands and crashes arrive as events with backtraces. Debugging: memory "
    "(read/search/watch/scan, probes), threads/backtraces, IPC/SVC tracing, gpu_profile, "
    "settings and debug knobs. To find what code touches memory or runs: breakpoint_add "
    "(exec/read/write watchpoints through the emulator's GDB stub; hits are 'break' events with "
    "backtraces). For rendering bugs: dump_set/dump_list/dump_read (shader and macro dumps). "
    "For crashes: report_list/report_read (with reporting_services on). To measure what a "
    "setting does: ab_test. State-changing tools need MCP writes (Tools > MCP Server > Allow "
    "Writes) and the control lease, granted automatically when no other agent holds it. Timing "
    "is not cycle-accurate: compare runs with tolerance.";

/// Keys must never reach an agent: in lines that mention a key, hex runs of 32+ digits (key or
/// key-derived material) are replaced.
std::string RedactKeys(std::string_view message) {
    std::string out{message};
    bool mentions_key = false;
    for (size_t i = 0; i + 3 <= out.size() && !mentions_key; ++i) {
        mentions_key = std::tolower(static_cast<unsigned char>(out[i])) == 'k' &&
                       std::tolower(static_cast<unsigned char>(out[i + 1])) == 'e' &&
                       std::tolower(static_cast<unsigned char>(out[i + 2])) == 'y';
    }
    if (!mentions_key) {
        return out;
    }
    for (size_t i = 0; i < out.size();) {
        size_t j = i;
        while (j < out.size() && std::isxdigit(static_cast<unsigned char>(out[j]))) {
            ++j;
        }
        if (j - i >= 32) {
            out.replace(i, j - i, "[redacted]");
            i += 10;
        } else {
            i = j + 1;
        }
    }
    return out;
}

/// External log sink: copies each log line, redacted, into the active timeline if there is one.
/// Called from whichever thread logs.
void LogSink(const char* class_name, Common::Log::Level level, std::string_view message) {
    if (Timeline* timeline = g_active_timeline.load(std::memory_order_acquire)) {
        timeline->Push(EventKind::Log, static_cast<u8>(level), class_name, RedactKeys(message));
    }
}

u64 CurrentProcessId() {
#ifdef _WIN32
    return static_cast<u64>(_getpid());
#else
    return static_cast<u64>(getpid());
#endif
}

/// Where the discovery file (url, port, pid) is written while the server runs.
std::filesystem::path DiscoveryPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "mcp-server.json";
}

std::string Lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

Timeline* Server::ActiveTimeline() {
    return g_active_timeline.load(std::memory_order_acquire);
}

/// Runs a tool directly, bypassing the protocol layer, and turns exceptions into errors.
ToolResult Server::CallTool(const std::string& name, const json& arguments) {
    const Tool* tool = registry.Find(name);
    if (!tool) {
        return ToolResult::Error("unknown tool " + name);
    }
    ToolContext ctx = MakeContext();
    if (tool->requires_game && !ctx.GameRunning()) {
        return ToolResult::Error("no game is running");
    }
    try {
        return tool->handler(ctx, arguments);
    } catch (const std::exception& e) {
        return ToolResult::Error(e.what());
    }
}

/// Current URL, session count, write state, activity and lease holder for the frontend.
Server::Summary Server::GetSummary() {
    Summary s{.url = Url(),
              .sessions = protocol->SessionCount(),
              .writes = writes_allowed.load(),
              .activity = Activity::Get().Snapshot()};
    if (const auto holder = control.Current()) {
        s.controller = holder->label.empty() ? holder->session_id.substr(0, 8) : holder->label;
    }
    return s;
}

Server::Server(Core::System& system_, Host& host_, ServerConfig config_)
    : system{system_}, host{host_}, config{std::move(config_)},
      writes_allowed{config.allow_writes}, timeline{ProcessTimeline()} {
    RegisterAllTools(registry);
    ProtocolOptions options;
    options.server_version = Common::g_build_fullname;
    options.instructions = kInstructions;

    // Resources: live views built from the tools, readable without a tool call.
    // run_tool returns the tool's structured result, or an {"error"} object if unavailable.
    const auto run_tool = [this](const char* name, const nlohmann::json& args, ToolContext& ctx) {
        const Tool* tool = registry.Find(name);
        if (!tool || (tool->requires_game && !ctx.GameRunning())) {
            return nlohmann::json{{"error", "unavailable (no game running?)"}};
        }
        ToolResult r = tool->handler(ctx, args);
        return r.structured ? *r.structured : nlohmann::json{{"error", "no data"}};
    };
    options.resources.push_back({
        .uri = "eglantine://tools/guide",
        .name = "Tool guide",
        .description = "Every tool with its description, grouped by what it is for.",
        .mime_type = "text/markdown",
        .read = [this](ToolContext&) {
            std::string text = "# Eglantine MCP tools\n\n" + std::string{kInstructions} + "\n\n";
            registry.ForEach([&](const Tool& t) {
                text += fmt::format("- **{}**{}{}: {}\n", t.name, t.requires_write ? " (writes)" : "",
                                    t.requires_game ? " (game)" : "", t.description);
            });
            return text;
        },
    });
    options.resources.push_back({
        .uri = "eglantine://status",
        .name = "Emulator status",
        .description = "Same as the status tool.",
        .read = [run_tool](ToolContext& ctx) {
            return run_tool("status", nlohmann::json::object(), ctx).dump(1);
        },
    });
    options.resources.push_back({
        .uri = "eglantine://game/current",
        .name = "Current game",
        .description = "Running game's metadata plus its saved screen labels, routes and probes.",
        .read = [run_tool](ToolContext& ctx) {
            return nlohmann::json{
                {"game", run_tool("game_info", {{"icon", false}}, ctx)},
                {"screen_labels", run_tool("screen_labels", nlohmann::json::object(), ctx)},
                {"routes", run_tool("route_list", nlohmann::json::object(), ctx)},
                {"probes", run_tool("probe_read", nlohmann::json::object(), ctx)},
            }
                .dump(1);
        },
    });
    options.resources.push_back({
        .uri = "eglantine://timeline/summary",
        .name = "Timeline digest",
        .description = "summarize_since over the whole timeline (warning+ clusters, crashes, "
                       "unimplemented commands).",
        .read = [run_tool](ToolContext& ctx) {
            return run_tool("summarize_since", {{"since_seq", 0}}, ctx).dump(1);
        },
    });

    // Prompts: canned workflows.
    options.prompts.push_back({
        .name = "boot_test",
        .description = "Boot a game and report how far it gets and what blocks it.",
        .arguments = nlohmann::json::array(
            {{{"name", "game"}, {"description", "Title or path"}, {"required", true}}}),
        .render = [](const nlohmann::json& a) {
            return fmt::format(
                "Boot-test '{}' on the Eglantine emulator through its MCP tools. Call status, then "
                "boot_game. If route_list has 'to_gameplay', run it; otherwise use screen_state, "
                "act and screenshots to get to gameplay, labelling screens (screen_label) and "
                "saving a route (route_save) as you go. Then report: furthest point reached, fps "
                "(status), summarize_since (warnings, crashes), unimplemented_commands with their "
                "backtraces, and a table of blockers: service/command, caller, likely expected "
                "reply.",
                a.value("game", "?"));
        },
    });
    options.prompts.push_back({
        .name = "investigate_unimplemented",
        .description = "Work out what an unimplemented service command should return.",
        .arguments = nlohmann::json::array(
            {{{"name", "service"}, {"description", "Service prefix, e.g. nx2:"}, {"required", false}}}),
        .render = [](const nlohmann::json& a) {
            return fmt::format(
                "List unimplemented_commands (service_prefix '{}'), most frequent first. For each, "
                "read first_request and first_backtrace: the SDK function names in the backtrace "
                "say what the caller wants. Use ipc_trace to see the surrounding calls and their "
                "results, read_memory on buffers it passes, and propose the reply (result code, "
                "output words) with your reasoning. Note which ones the game keeps polling "
                "(ipc_summary) since those likely block progress.",
                a.value("service", ""));
        },
    });
    options.prompts.push_back({
        .name = "perf_check",
        .description = "Measure where the frame time goes in the current scene.",
        .render = [](const nlohmann::json&) {
            return std::string{
                "Enable gpu_profile with window_ms 3000 and fresh true. Report fps, the top zones "
                "on the GPU front thread ('GPU thread') and back thread ('GPU back thread'), split "
                "drains per second, draws per second and ms per draw, then list_threads twice 5 s "
                "apart to get guest CPU time per thread. Say whether the bottleneck is the GPU "
                "front, the back, the guest CPU, or waiting between them, with numbers."};
        },
    });
    options.prompts.push_back({
        .name = "compare_change",
        .description = "Show what a setting/knob/code change did, with a repeatable route.",
        .arguments = nlohmann::json::array(
            {{{"name", "route"}, {"description", "Route to replay"}, {"required", true}}}),
        .render = [](const nlohmann::json& a) {
            return fmt::format(
                "Mark the timeline, run route '{0}', timeline_export 'before'. Apply the change "
                "(settings_set, debug_knobs, or a rebuild + relaunch), run route '{0}' again, "
                "timeline_export 'after', then compare_runs before/after and compare screenshots "
                "and gpu_profile numbers. Report only differences that repeat.",
                a.value("route", "to_gameplay"));
        },
    });
    options.prompts.push_back({
        .name = "find_writer",
        .description = "Find the code that changes a value in the game's memory.",
        .arguments = nlohmann::json::array(
            {{{"name", "what"}, {"description", "The value, e.g. 'player health'"}, {"required", true}}}),
        .render = [](const nlohmann::json& a) {
            return fmt::format(
                "Find which code changes {0}. 1) Locate it: scan_start with the current value, "
                "change it in the game (act), scan_next, repeat until a few addresses remain; "
                "confirm with watch_add. 2) Check debugger_status: if the GDB stub is not running, "
                "settings_set use_gdbstub true, stop and boot the game again (route_run "
                "'to_gameplay' if it exists) and find the address again (it can move). 3) "
                "breakpoint_add type write on the address (mode log), make the game change it, "
                "then wait_for kinds ['break'] or debugger_status recent_hits: report the writing "
                "function (Module+offset and symbol), its callers, and the registers. 4) "
                "breakpoint_remove all and debugger_detach when done.",
                a.value("what", "the value"));
        },
    });
    options.prompts.push_back({
        .name = "render_bug",
        .description = "Narrow down a rendering problem to shaders, validation errors or settings.",
        .render = [](const nlohmann::json&) {
            return std::string{
                "Screenshot the problem. Then: 1) dump_set shaders true and spirv true, reach the "
                "broken scene again, and dump_list since_ms_ago to see which shaders it compiled. "
                "2) For Vulkan validation: settings_set renderer_debug true, stop and boot the "
                "game again, reproduce, then summarize_since / timeline with source_prefix "
                "'Render.Vulkan' for the validation messages that repeat. 3) ab_test the "
                "graphics settings that could matter (e.g. disable_macro_jit, "
                "disable_buffer_reorder, gpu accuracy) with a screenshot each, and report which "
                "change the picture. Turn the debug settings back off afterwards."};
        },
    });
    options.bearer_token = config.bearer_token;
    options.on_session_end = [](const std::string& session_id) {
        StopSessionRoutes(session_id);
        StopSessionAbTest(session_id);
    };
    protocol = std::make_unique<Protocol>(registry, std::move(options), [this] { return MakeContext(); });
}

Server::~Server() {
    Stop();
}

ToolContext Server::MakeContext() {
    return ToolContext{&system,  &host, &timeline, writes_allowed.load(),
                       &control, {},    &writes_allowed};
}

std::string Server::Url() const {
    return fmt::format("http://{}:{}/mcp", config.bind_address, config.port);
}

/// Sets up the HTTP routes, binds the port and starts listening; returns an error or empty.
std::string Server::Start() {
    if (running) {
        return {};
    }
    http = std::make_unique<httplib::Server>();
    // Long waits (wait_for, act, /events/wait) each hold a worker: leave room for normal calls.
    http->new_task_queue = [] { return new httplib::ThreadPool(8); };
    http->set_payload_max_length(2 << 20);
    // httplib's default (SO_REUSEADDR on Windows, SO_REUSEPORT elsewhere) lets a second emulator
    // bind the same port, and both then serve it. Take the port exclusively instead, so a second
    // instance reports "port in use".
    http->set_socket_options([](auto sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));
#endif
    });
    // httplib's 5 s defaults cut large replies (screenshots) when something inspects loopback
    // traffic (e.g. security software): clients saw connection resets mid-body.
    http->set_read_timeout(60, 0);
    http->set_write_timeout(60, 0);

    // Converts an httplib request (headers lowercased) to HttpRequest, runs it through Protocol
    // and copies the result back. Runs on an httplib worker thread.
    const auto handler = [this](const httplib::Request& req, httplib::Response& res) {
        HttpRequest request;
        request.method = req.method;
        request.path = req.path;
        request.body = req.body;
        for (const auto& [key, value] : req.headers) {
            request.headers.emplace(Lowercase(key), value);
        }
        const HttpResponse response = protocol->Handle(request);
        res.status = response.status;
        for (const auto& [key, value] : response.headers) {
            res.set_header(key, value);
        }
        if (!response.body.empty()) {
            res.set_content(response.body, response.content_type.c_str());
        }
    };
    // GET /events/wait?kinds=crash,unimpl&contains=..&source_prefix=..&min_level=..&timeout_s=..
    // Blocks until a matching timeline event happens after the request (or the timeout) and
    // returns it as JSON. Made for a background `curl` an agent starts and gets woken by when it
    // exits, without holding an MCP call open.
    // Same checks as /mcp for the other routes (the token may also come as ?token= for browsers).
    const auto authorized = [this](const httplib::Request& req, httplib::Response& res) {
        if (!Protocol::IsAllowedHost(req.get_header_value("Host"))) {
            res.status = 403;
            return false;
        }
        if (const auto origin = req.get_header_value("Origin");
            !origin.empty() && !Protocol::IsAllowedOrigin(origin)) {
            res.status = 403;
            return false;
        }
        if (!config.bearer_token.empty() &&
            req.get_header_value("Authorization") != "Bearer " + config.bearer_token &&
            req.get_param_value("token") != config.bearer_token) {
            res.status = 401;
            return false;
        }
        return true;
    };
    // Timeline filter from ?since_seq&kinds=a,b&contains&source_prefix&min_level.
    const auto parse_query = [this](const httplib::Request& req) {
        Timeline::Query q;
        q.since_seq = timeline.LastSeq();
        if (req.has_param("since_seq")) {
            q.since_seq = std::strtoull(req.get_param_value("since_seq").c_str(), nullptr, 10);
        }
        const std::string kinds = req.get_param_value("kinds");
        for (size_t start = 0; start < kinds.size();) {
            const size_t comma = std::min(kinds.find(',', start), kinds.size());
            if (const auto kind = EventKindFromName(kinds.substr(start, comma - start))) {
                q.kinds.push_back(*kind);
            }
            start = comma + 1;
        }
        q.contains = req.get_param_value("contains");
        q.source_prefix = req.get_param_value("source_prefix");
        if (req.has_param("min_level")) {
            static constexpr std::string_view levels[] = {"trace", "debug", "info",
                                                          "warning", "error", "critical"};
            for (int i = 0; i < 6; ++i) {
                if (levels[i] == req.get_param_value("min_level")) {
                    q.min_level = i;
                }
            }
        }
        return q;
    };
    // Server-sent events: calls next(q) for batches of matching events, pinging every 15 s.
    const auto stream_events = [this](httplib::Response& res, Timeline::Query q,
                                      std::function<std::string(const Event&)> format) {
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, q, format](size_t, httplib::DataSink& sink) mutable {
                if (!running) {
                    return false;
                }
                if (!timeline.WaitFor(q, std::chrono::seconds{15})) {
                    return sink.write(": ping\n\n", 8);
                }
                q.limit = 500;
                const auto r = timeline.Read(q);
                for (const auto& e : r.events) {
                    const std::string text = "data: " + format(e) + "\n\n";
                    if (!sink.write(text.data(), text.size())) {
                        return false;
                    }
                }
                q.since_seq = r.next_seq;
                return true;
            });
    };
    // One timeline event as JSON; log events also carry their level name.
    const auto event_json = [](const Event& e) {
        nlohmann::json j{{"seq", e.seq},       {"frame", e.frame},     {"kind", EventKindName(e.kind)},
                         {"source", e.source}, {"msg", e.message}};
        if (e.kind == EventKind::Log) {
            static constexpr const char* levels[] = {"trace", "debug", "info",
                                                     "warning", "error", "critical"};
            j["level"] = e.level < 6 ? levels[e.level] : "?";
        }
        return j;
    };

    // GET /stream/timeline?<filter>: the timeline as SSE (dashboard, external recorders).
    http->Get("/stream/timeline",
              [=](const httplib::Request& req, httplib::Response& res) {
                  if (authorized(req, res)) {
                      stream_events(res, parse_query(req),
                                    [=](const Event& e) { return event_json(e).dump(); });
                  }
              });

    // GET /mcp with Accept: text/event-stream and a session: the MCP server-to-client stream.
    // Pushes notifications/message for crashes, new unimplemented commands and warning+ logs.
    http->Get("/mcp", [=, this](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) {
            return;
        }
        const std::string session = req.get_header_value("Mcp-Session-Id");
        if (req.get_header_value("Accept").find("text/event-stream") == std::string::npos ||
            session.empty() || !protocol->HasSession(session)) {
            handler(req, res); // protocol rules: 405 without a stream, 400/404 for sessions
            return;
        }
        Timeline::Query q;
        q.since_seq = timeline.LastSeq();
        q.kinds = {EventKind::Log, EventKind::Crash, EventKind::Unimplemented};
        q.min_level = 3;
        stream_events(res, q, [=](const Event& e) {
            const char* level = e.kind == EventKind::Crash ? "critical"
                                : e.kind == EventKind::Unimplemented ? "warning"
                                : e.level >= 5 ? "critical"
                                : e.level == 4 ? "error"
                                               : "warning";
            return nlohmann::json{{"jsonrpc", "2.0"},
                                  {"method", "notifications/message"},
                                  {"params", {{"level", level},
                                              {"logger", e.source},
                                              {"data", event_json(e)}}}}
                .dump();
        });
    });

    // Dashboard and its data (read-only).
    http->Get("/", [=](const httplib::Request& req, httplib::Response& res) {
        if (authorized(req, res)) {
            res.set_content(kDashboardHtml, "text/html; charset=utf-8");
        }
    });
    http->Get("/status.json", [=, this](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) {
            return;
        }
        ToolContext ctx = MakeContext();
        nlohmann::json out = nlohmann::json::object();
        if (const Tool* status = registry.Find("status")) {
            if (auto r = status->handler(ctx, nlohmann::json::object()); r.structured) {
                out = *r.structured;
            }
        }
        // The screen label costs a frame capture: only when asked (?screen=1).
        if (ctx.GameRunning() && req.get_param_value("screen") == "1") {
            std::string error;
            if (const auto state = GameScreenState(ctx, error); !state.is_null()) {
                out["screen"] = state["screen"];
            }
        }
        if (const auto holder = control.Current()) {
            out["control"] = holder->label.empty() ? holder->session_id.substr(0, 8) : holder->label;
        }
        const auto a = Activity::Get().Snapshot();
        if (a.tool_calls > 0) {
            out["last_tool"] = fmt::format(
                "{} ({} s ago)", a.last_tool,
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                 a.last_tool_time)
                    .count());
        }
        if (a.recording) {
            out["recording"] = fmt::format("{}: {} frames, {} presses", a.record_name,
                                           a.record_frames, a.record_checkpoints);
        }
        if (!a.route_state.empty() &&
            (a.route_state == "running" ||
             std::chrono::steady_clock::now() - a.route_changed < std::chrono::seconds{30})) {
            out["route"] = fmt::format("{} {}/{} {} {}", a.route, a.route_step, a.route_total,
                                       a.route_state, a.route_detail);
        }
        // Debugger, A/B test and recent reports for the dashboard's lower panels (?extras=1,
        // polled less often: the report listing reads directories).
        if (req.get_param_value("extras") == "1") {
            const auto run = [&](const char* tool, const nlohmann::json& args) -> nlohmann::json {
                const Tool* t = registry.Find(tool);
                if (!t) {
                    return nullptr;
                }
                try {
                    const auto r = t->handler(ctx, args);
                    return r.structured ? *r.structured : nlohmann::json(nullptr);
                } catch (const std::exception&) {
                    return nullptr;
                }
            };
            out["extras"] = {{"debugger", run("debugger_status", nlohmann::json::object())},
                             {"ab_test", run("ab_test_status", nlohmann::json::object())},
                             {"reports", run("report_list", {{"limit", 5}})}};
        }
        res.set_content(out.dump(), "application/json");
    });
    http->Get("/screen.png", [=, this](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) {
            return;
        }
        const u32 width = std::clamp<u32>(
            req.has_param("w") ? static_cast<u32>(std::atoi(req.get_param_value("w").c_str())) : 640,
            160, 1920);
        std::string error;
        if (const auto png = host.CaptureScreenshotPng(width, std::chrono::milliseconds{1000}, error)) {
            res.set_content(std::string{png->begin(), png->end()}, "image/png");
        } else {
            res.status = 503;
            res.set_content(error, "text/plain");
        }
    });

    http->Get("/events/wait", [=, this](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) {
            return;
        }
        Timeline::Query q = parse_query(req);
        const long timeout_s = std::clamp(
            req.has_param("timeout_s") ? std::atol(req.get_param_value("timeout_s").c_str()) : 600L,
            1L, 86400L);
        // Wait in slices so a server stop isn't held up for long.
        std::optional<Event> found;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout_s};
        while (running && !found && std::chrono::steady_clock::now() < deadline) {
            found = timeline.WaitFor(q, std::chrono::seconds{1});
        }
        nlohmann::json out;
        if (found) {
            out = {{"matched", true},
                   {"seq", found->seq},
                   {"frame", found->frame},
                   {"kind", EventKindName(found->kind)},
                   {"source", found->source},
                   {"msg", found->message}};
        } else {
            out = {{"matched", false}, {"next_seq", timeline.LastSeq()}};
        }
        res.set_content(out.dump(1), "application/json");
    });
    http->Post(".*", handler);
    http->Get(".*", handler);
    http->Delete(".*", handler);

    if (!http->bind_to_port(config.bind_address, config.port)) {
        http.reset();
        const std::string error =
            fmt::format("could not listen on {}:{} (port in use?)", config.bind_address, config.port);
        LOG_ERROR(Debug_MCP, "MCP server: {}", error);
        return error;
    }
    running = true;
    g_server_stopping.store(false, std::memory_order_relaxed);
    timeline.Reopen();
    g_active_timeline.store(&timeline, std::memory_order_release);
    Common::Log::SetExternalSink(&LogSink);
    InstallTraceHooks();
    http_thread = std::thread([this] { http->listen_after_bind(); });
    // Discovery file so scripts find a running instance (never contains the token).
    {
        const nlohmann::json discovery{{"url", Url()},
                                       {"port", config.port},
                                       {"pid", CurrentProcessId()},
                                       {"frontend", host.FrontendName()},
                                       {"token_required", !config.bearer_token.empty()},
                                       {"writes_enabled", writes_allowed.load()}};
        const std::string text = discovery.dump(1);
        if (Common::FS::WriteStringToFile(DiscoveryPath(), Common::FS::FileType::TextFile, text) !=
            text.size()) {
            LOG_WARNING(Debug_MCP, "Could not write the discovery file {}",
                        Common::FS::PathToUTF8String(DiscoveryPath()));
        }
    }
    timeline.Push(EventKind::State, 0, "mcp", "MCP server listening on " + Url());
    LOG_INFO(Debug_MCP, "MCP server listening on {}", Url());
    return {};
}

/// Wakes waiting requests, removes the hooks and discovery file, and stops the HTTP thread.
void Server::Stop() {
    if (!running.exchange(false)) {
        return;
    }
    // Wake every request waiting on the timeline (wait_for, act, SSE streams, /events/wait) and
    // end tools' own waits, so the HTTP workers finish now instead of at their timeouts.
    g_server_stopping.store(true, std::memory_order_relaxed);
    timeline.Close();
    Common::Log::SetExternalSink(nullptr);
    RemoveTraceHooks();
    StopGameStateWork();
    StopMemoryWork();
    StopAbTestWork();
    // Leaves no breakpoints behind and lets a game the debugger paused run again.
    StopDebuggerWork();
    std::error_code ec;
    std::filesystem::remove(DiscoveryPath(), ec);
    g_active_timeline.store(nullptr, std::memory_order_release);
    http->stop();
    if (http_thread.joinable()) {
        http_thread.join();
    }
    http.reset();
    LOG_INFO(Debug_MCP, "MCP server stopped");
}

} // namespace Mcp
