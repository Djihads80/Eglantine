// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A/B tests of a setting: sets each value in turn (rebooting the game when the setting cannot
// change live), measures with perf_sample or a saved benchmark route, restores the original value
// and returns a comparison table.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "mcp/activity.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

using namespace std::chrono;

/// Guest frames to wait for after a reboot before the game counts as up.
constexpr u64 kBootFrames = 120;

// ---- settings (shared with settings_set: same hidden-settings rule and parsing) -------------

/// Looks up a setting agents may change; nullptr if unknown or hidden.
Settings::BasicSetting* FindSetting(const std::string& name) {
    return FindAgentSetting(name);
}

/// Sets a setting from text exactly like settings_set.
std::string SetFromString(Settings::BasicSetting& s, const std::string& value) {
    return SetSettingFromText(s, value);
}

/// The value as settings_get shows it (enum values by name).
std::string DisplayValue(const Settings::BasicSetting& s) {
    return s.IsEnum() ? s.Canonicalize() : s.ToString();
}

// ---- test state -----------------------------------------------------------------------------

/// One ab_test's arguments, checked before it starts.
struct AbConfig {
    std::string setting;
    std::vector<std::string> values;
    std::string route;      ///< Saved route measured per value, or "" for a plain perf_sample
    std::string boot_route; ///< Saved route run after each reboot, or ""
    int sample_seconds = 8;
    bool reboot = false;   ///< Reboot the game after setting each value
    bool not_live = false; ///< The setting is not runtime-modifiable
    std::string game_path; ///< File booted again when rebooting
};

/// The current or last ab_test (blocking or background); fields are guarded by `mutex`.
struct AbTestJob {
    std::mutex mutex;
    std::jthread thread;
    std::stop_source stop_source;
    std::string state; ///< running / done / failed / stopped, empty before the first test
    bool background = false;
    std::string setting;
    std::vector<std::string> values;
    size_t index = 0;
    std::string phase;
    json rows = json::array(); ///< Table rows finished so far
    json result;               ///< Full result once finished
    steady_clock::time_point started{};
    std::string session; ///< MCP session that started it
};

/// The process-wide ab_test slot.
AbTestJob& Job() {
    static AbTestJob job;
    return job;
}

/// Shows progress in the status bar (Activity), in ab_test_status and, when `event` is set, on
/// the timeline. An index past the last value reports a phase of the whole test (restoring).
void Report(ToolContext& ctx, const AbConfig& cfg, size_t index, const std::string& phase,
            bool event = true) {
    const std::string detail =
        index < cfg.values.size()
            ? fmt::format("value {} ({}/{}): {}", cfg.values[index], index + 1, cfg.values.size(),
                          phase)
            : phase;
    Activity::Get().Update([&](ActivityState& s) {
        s.route = "ab_test " + cfg.setting;
        s.route_step = std::min(index, cfg.values.size());
        s.route_total = cfg.values.size();
        s.route_state = "running";
        s.route_detail = detail;
        s.route_changed = steady_clock::now();
    });
    {
        auto& job = Job();
        std::scoped_lock lock{job.mutex};
        job.index = index;
        job.phase = phase;
    }
    if (event && ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "ab_test", "ab_test: " + detail);
    }
}

/// Why the test may no longer drive the game, or "" while it may. Like the route runner: a stop
/// request, server shutdown, the game stopping (when `need_game`) and, for an agent's session,
/// Allow Writes turned off or the control lease lost. Each check renews the lease.
std::string Refusal(const ToolContext& ctx, const std::stop_token* stop, bool need_game) {
    if (ServerStopping()) {
        return "the MCP server is stopping";
    }
    if (stop != nullptr && stop->stop_requested()) {
        return "stopped by request (ab_test_stop)";
    }
    if (need_game && !ctx.GameRunning()) {
        return "the game stopped";
    }
    if (ctx.session_id.empty()) {
        return {};
    }
    if (ctx.live_writes != nullptr && !ctx.live_writes->load()) {
        return "Allow Writes was turned off";
    }
    if (ctx.control != nullptr && !ctx.control->Renew(ctx.session_id)) {
        return "lost the control lease";
    }
    return {};
}

/// Asked while waiting: returns why to give up, or "" to keep waiting.
using RefusalFn = std::function<std::string(bool need_game)>;

/// Polls `done` every 100 ms for up to `timeout`. Returns "" once it holds, else the refusal
/// reason or a timeout message naming `what`.
std::string WaitUntil(const std::function<bool()>& done, milliseconds timeout,
                      const RefusalFn& refusal, bool need_game, std::string_view what) {
    const auto deadline = steady_clock::now() + timeout;
    while (!done()) {
        if (std::string reason = refusal(need_game); !reason.empty()) {
            return reason;
        }
        if (steady_clock::now() >= deadline) {
            return fmt::format("timed out after {} s waiting for {}",
                               duration_cast<seconds>(timeout).count(), what);
        }
        std::this_thread::sleep_for(milliseconds{100});
    }
    return {};
}

/// Stops the game, boots `path` again and waits until it presents kBootFrames frames. Returns
/// "" or what went wrong.
std::string Reboot(ToolContext& ctx, const std::string& path, const RefusalFn& refusal) {
    if (ctx.host == nullptr) {
        return "no frontend host is attached";
    }
    if (ctx.GameRunning()) {
        if (std::string error = ctx.host->Stop(); !error.empty()) {
            return "stop failed: " + error;
        }
    }
    if (std::string error = WaitUntil([&] { return !ctx.GameRunning(); }, seconds{60}, refusal,
                                      false, "the game to stop");
        !error.empty()) {
        return error;
    }
    std::string boot_error;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (std::string reason = refusal(false); !reason.empty()) {
            return reason;
        }
        boot_error = ctx.host->BootGame(path);
        // The frontend may still be tearing the last session down for a moment.
        if (boot_error.empty() || boot_error.find("already running") == std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(milliseconds{500});
    }
    if (!boot_error.empty()) {
        return "boot failed: " + boot_error;
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "ab_test", "ab_test: boot requested: " + path);
    }
    if (std::string error = WaitUntil([&] { return ctx.GameRunning(); }, seconds{120}, refusal,
                                      false, "the game to start");
        !error.empty()) {
        return error;
    }
    u64 start = PresentedFrames();
    return WaitUntil(
        [&] {
            const u64 now = PresentedFrames();
            start = std::min(start, now); // The counter may restart with the new session.
            return now - start >= kBootFrames;
        },
        seconds{300}, refusal, true, "the rebooted game to present frames");
}

/// File of the running game, found by its program id in the frontend's game list. Returns ""
/// with `error` set when it cannot be determined; `note` mentions an ambiguous match.
std::string RunningGamePath(ToolContext& ctx, std::string& error, std::string& note) {
    if (ctx.host == nullptr) {
        error = "no frontend host is attached, so the game cannot be rebooted";
        return {};
    }
    const auto games = ctx.host->ListGames();
    if (!games) {
        error = "this frontend has no game list, so ab_test cannot find the running game's file "
                "to reboot it; pass reboot:\"never\" for runtime-modifiable settings";
        return {};
    }
    const u64 program_id = ctx.system->GetApplicationProcessProgramID();
    std::vector<const GameEntry*> matches;
    for (const auto& game : *games) {
        if (game.program_id == program_id) {
            matches.push_back(&game);
        }
    }
    if (matches.empty()) {
        error = fmt::format("the running game ({:016X}) is not in the game list, so ab_test "
                            "cannot reboot it; add its folder to the game list (list_games) or "
                            "pass reboot:\"never\"",
                            program_id);
        return {};
    }
    if (matches.size() > 1) {
        note = fmt::format("{} game list entries share program id {:016X}; rebooting {}",
                           matches.size(), program_id, matches.front()->path);
    }
    return matches.front()->path;
}

// ---- measuring ------------------------------------------------------------------------------

/// Rounds to one decimal.
double Round1(double v) {
    return std::round(v * 10) / 10;
}

/// Averages perf_sample results that saw frames into one table row's figures (null when none).
/// `count` is set to how many samples were used.
json SummarizeSamples(const json& samples, size_t& count) {
    double fps = 0, low = 0, median = 0, p95 = 0, speed = 0;
    size_t speed_count = 0;
    count = 0;
    for (const auto& s : samples) {
        if (!s.is_object() || !s.contains("fps") || s.value("frames", u64{0}) == 0) {
            continue;
        }
        const json& f = s["fps"];
        fps += f.value("average", 0.0);
        low += f.value("one_percent_low_fps", 0.0);
        median += f.value("frame_ms_median", 0.0);
        p95 += f.value("frame_ms_p95", 0.0);
        ++count;
        if (s.contains("emulation_speed_pct") && s["emulation_speed_pct"].is_number()) {
            speed += s["emulation_speed_pct"].get<double>();
            ++speed_count;
        }
    }
    json out{{"fps_avg", nullptr},
             {"one_percent_low", nullptr},
             {"frame_ms_median", nullptr},
             {"frame_ms_p95", nullptr},
             {"speed_pct", nullptr}};
    if (count > 0) {
        const double n = static_cast<double>(count);
        out["fps_avg"] = Round1(fps / n);
        out["one_percent_low"] = Round1(low / n);
        out["frame_ms_median"] = Round1(median / n);
        out["frame_ms_p95"] = Round1(p95 / n);
    }
    if (speed_count > 0) {
        out["speed_pct"] = Round1(speed / static_cast<double>(speed_count));
    }
    return out;
}

/// Where a failed route run stopped, from its step results.
std::string RouteFailure(const json& results) {
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        const json& r = *it;
        const auto step = r.value("step", 0);
        const std::string label = r.contains("label") ? " (" + r["label"].dump() + ")" : "";
        if (r.contains("stopped")) {
            return fmt::format("stopped at step {}{}: {}", step, label,
                               r.value("stopped", std::string{}));
        }
        if (r.contains("error")) {
            return fmt::format("error at step {}{}: {}", step, label,
                               r.value("error", std::string{}));
        }
        if (r.value("timed_out", false)) {
            return fmt::format("timed out at step {}{}", step, label);
        }
    }
    return "see the route_status / timeline events";
}

/// One perf_sample of cfg.sample_seconds at the current point, labelled with the value.
json SampleHere(ToolContext& ctx, const AbConfig& cfg, size_t index) {
    Report(ctx, cfg, index, fmt::format("measuring for {} s", cfg.sample_seconds));
    std::string label = fmt::format("ab_test {}={}", cfg.setting, cfg.values[index]);
    label.resize(std::min<size_t>(label.size(), 100));
    return RunPerfSample(ctx, json{{"seconds", cfg.sample_seconds},
                                   {"label", label},
                                   {"profile", false},
                                   {"screenshot", false}});
}

/**
 * Sets value `index`, reboots and runs boot_route if configured, then measures it.
 * @param row     Table row to fill (value and route_ok are preset; figures are added here).
 * @param entry   Raw-sample entry to fill.
 * @param booted  Set once the game was (or may have been) rebooted with a test value.
 * @param aborted Set to why the whole test must end; empty to continue with the next value.
 */
void MeasureValue(ToolContext& ctx, const AbConfig& cfg, size_t index,
                  Settings::BasicSetting& setting, std::stop_token stop, json& row, json& entry,
                  std::vector<std::string>& notes, bool& booted, std::string& aborted) {
    const RefusalFn refusal = [&](bool need_game) { return Refusal(ctx, &stop, need_game); };
    const std::string& value = cfg.values[index];
    Report(ctx, cfg, index, "setting the value");
    if (std::string error = SetFromString(setting, value); !error.empty()) {
        notes.push_back(error);
        return;
    }
    const std::string expected = setting.ToString();
    entry["applied"] = DisplayValue(setting);
    if (ctx.host != nullptr && ctx.GameRunning()) {
        ctx.host->ApplySettings();
    }

    if (cfg.reboot) {
        Report(ctx, cfg, index, "rebooting the game");
        booted = true;
        if (std::string error = Reboot(ctx, cfg.game_path, refusal); !error.empty()) {
            notes.push_back("reboot failed: " + error);
            aborted = "reboot failed: " + error;
            return;
        }
        if (setting.ToString() != expected) {
            notes.push_back(fmt::format("the reboot changed the value to {} (per-game "
                                        "configuration?): these figures are for that value",
                                        DisplayValue(setting)));
        }
        if (!cfg.boot_route.empty()) {
            Report(ctx, cfg, index, "running boot_route " + cfg.boot_route);
            std::string error;
            const json r = RunRouteForTool(ctx, cfg.boot_route, stop, error);
            if (r.is_null()) {
                notes.push_back("boot_route: " + error);
                aborted = "boot_route: " + error;
                return;
            }
            entry["boot_route"] = {{"ok", r["ok"]}, {"elapsed_ms", r["elapsed_ms"]}};
            if (!r["ok"].get<bool>()) {
                notes.push_back(fmt::format("boot_route {} failed, not measured: {}",
                                            cfg.boot_route, RouteFailure(r["results"])));
                if (std::string reason = refusal(true); !reason.empty()) {
                    aborted = reason;
                }
                return;
            }
        }
    } else if (cfg.not_live) {
        notes.push_back("not runtime-modifiable and reboot is \"never\": the game may still use "
                        "the previous value");
    }

    if (std::string reason = refusal(true); !reason.empty()) {
        aborted = reason;
        return;
    }
    json samples = json::array();
    if (!cfg.route.empty()) {
        Report(ctx, cfg, index, "running route " + cfg.route);
        std::string error;
        const json r = RunRouteForTool(ctx, cfg.route, stop, error);
        if (r.is_null()) {
            notes.push_back("route: " + error);
            aborted = "route: " + error;
            return;
        }
        const bool ok = r["ok"].get<bool>();
        row["route_ok"] = ok;
        entry["route_elapsed_ms"] = r["elapsed_ms"];
        for (const auto& step : r["results"]) {
            if (step.contains("sample")) {
                json sample = step["sample"];
                sample["route_step"] = step.value("step", 0);
                samples.push_back(std::move(sample));
            }
        }
        if (!ok) {
            notes.push_back("route failed: " + RouteFailure(r["results"]));
        } else if (samples.empty()) {
            if (std::string reason = refusal(true); !reason.empty()) {
                aborted = reason;
                return;
            }
            notes.push_back(fmt::format("route has no sample steps: measured {} s at its end",
                                        cfg.sample_seconds));
            samples.push_back(SampleHere(ctx, cfg, index));
        }
    } else {
        samples.push_back(SampleHere(ctx, cfg, index));
    }

    size_t used = 0;
    const json figures = SummarizeSamples(samples, used);
    for (const auto& [key, v] : figures.items()) {
        row[key] = v;
    }
    if (used == 0) {
        notes.push_back("no frames were presented while measuring (paused or loading?)");
    } else if (used > 1) {
        notes.push_back(fmt::format("mean of {} samples", used));
    }
    entry["samples"] = std::move(samples);
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "ab_test",
                           fmt::format("ab_test: value {}: {} fps avg", value,
                                       row["fps_avg"].is_null() ? "no" : row["fps_avg"].dump()));
    }
}

/// Runs the whole test on the calling thread, then always restores the original value (and
/// reboots back when a test value was booted and the test may still drive the game). Catches
/// everything: an exception escaping a background thread would terminate the emulator.
json RunAbTest(ToolContext ctx, const AbConfig& cfg, std::stop_token stop) {
    ctx.stop = stop; // routes and input steps it runs end early when it is stopped
    const auto started = steady_clock::now();
    Settings::BasicSetting* setting = FindSetting(cfg.setting);
    if (setting == nullptr) {
        return json{{"state", "failed"}, {"aborted", "unknown or hidden setting"}};
    }
    const std::string original_raw = setting->ToString();
    const std::string original = DisplayValue(*setting);
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "ab_test",
                           fmt::format("ab_test: {} started ({} values, original {})",
                                       cfg.setting, cfg.values.size(), original));
    }

    json table = json::array();
    json raw = json::array();
    std::string aborted;
    bool booted = false;
    try {
        for (size_t i = 0; i < cfg.values.size() && aborted.empty(); ++i) {
            if (std::string reason = Refusal(ctx, &stop, true); !reason.empty()) {
                aborted = reason;
                break;
            }
            json row{{"value", cfg.values[i]},
                     {"fps_avg", nullptr},
                     {"one_percent_low", nullptr},
                     {"frame_ms_median", nullptr},
                     {"frame_ms_p95", nullptr},
                     {"speed_pct", nullptr},
                     {"route_ok", nullptr}};
            json entry{{"value", cfg.values[i]}, {"samples", json::array()}};
            std::vector<std::string> notes;
            MeasureValue(ctx, cfg, i, *setting, stop, row, entry, notes, booted, aborted);
            std::string joined;
            for (const auto& note : notes) {
                joined += (joined.empty() ? "" : "; ") + note;
            }
            row["notes"] = joined;
            table.push_back(row);
            raw.push_back(std::move(entry));
            auto& job = Job();
            std::scoped_lock lock{job.mutex};
            job.rows = table;
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Debug_MCP, "ab_test {} failed: {}", cfg.setting, e.what());
        aborted = std::string{"internal error: "} + e.what();
    } catch (...) {
        aborted = "internal error";
    }

    // Restore even after a failure or a stop request.
    std::string restore_note;
    try {
        setting->LoadString(original_raw);
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "ab_test",
                               fmt::format("ab_test: {} restored to {}", cfg.setting, original));
        }
        if (!ServerStopping() && ctx.host != nullptr && ctx.GameRunning()) {
            ctx.host->ApplySettings();
        }
        if (booted) {
            // Not tied to the stop token: a stopped test still reboots back to the original.
            const RefusalFn refusal = [&](bool need_game) {
                return Refusal(ctx, nullptr, need_game);
            };
            if (std::string why = refusal(true); !why.empty()) {
                restore_note = "not rebooted back (" + why + "): the original value applies on "
                               "the next boot";
            } else {
                Report(ctx, cfg, cfg.values.size(), "rebooting with the original value");
                if (std::string error = Reboot(ctx, cfg.game_path, refusal); !error.empty()) {
                    restore_note = "rebooting with the original value failed (" + error +
                                   "): it applies on the next boot";
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Debug_MCP, "ab_test {} restore failed: {}", cfg.setting, e.what());
        restore_note = std::string{"restoring failed: "} + e.what();
    } catch (...) {
        restore_note = "restoring failed";
    }

    json best = nullptr;
    for (const auto& row : table) {
        if (row["fps_avg"].is_number() &&
            (best.is_null() || row["fps_avg"].get<double>() > best["fps_avg"].get<double>())) {
            best = json{{"value", row["value"]}, {"fps_avg", row["fps_avg"]}};
        }
    }
    const char* state = stop.stop_requested()              ? "stopped"
                        : !aborted.empty() || best.is_null() ? "failed"
                                                             : "done";
    json out{{"state", state},
             {"setting", cfg.setting},
             {"original", original},
             {"rebooted", cfg.reboot},
             {"table", std::move(table)},
             {"best", best},
             {"samples", std::move(raw)},
             {"elapsed_ms", duration_cast<milliseconds>(steady_clock::now() - started).count()}};
    if (!aborted.empty()) {
        out["aborted"] = aborted;
    }
    if (!restore_note.empty()) {
        out["restore_note"] = restore_note;
    }
    const std::string summary =
        best.is_null() ? fmt::format("ab_test: {} {}", cfg.setting, state)
                       : fmt::format("ab_test: {} {}, best {} ({} fps avg)", cfg.setting, state,
                                     best["value"].get<std::string>(), best["fps_avg"].dump());
    Activity::Get().Update([&](ActivityState& s) {
        s.route = "ab_test " + cfg.setting;
        s.route_step = cfg.values.size();
        s.route_total = cfg.values.size();
        s.route_state = state;
        s.route_detail = aborted.empty() ? summary : aborted;
        s.route_changed = steady_clock::now();
    });
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "ab_test", summary);
    }
    return out;
}

/// Records a finished test's result in the job slot.
void FinishJob(const json& result) {
    auto& job = Job();
    std::scoped_lock lock{job.mutex};
    job.state = result.value("state", "failed");
    job.result = result;
    if (result.contains("table")) {
        job.rows = result["table"];
    }
}

// ---- tools ----------------------------------------------------------------------------------

/// ab_test tool: checks the arguments and preconditions, then runs the test on the calling
/// thread (blocking; is_error set when it failed) or, with `background`, on the ab_test thread.
ToolResult AbTest(ToolContext& ctx, const json& args) {
    AbConfig cfg;
    cfg.setting = args.at("setting").get<std::string>();
    for (const auto& v : args.at("values")) {
        cfg.values.push_back(v.get<std::string>());
    }
    cfg.route = args.value("route", "");
    cfg.boot_route = args.value("boot_route", "");
    cfg.sample_seconds = static_cast<int>(std::lround(args.value("sample_seconds", 8.0)));
    const std::string reboot_mode = args.value("reboot", "auto");
    const bool background = args.value("background", false);

    auto& job = Job();
    {
        std::scoped_lock lock{job.mutex};
        if (job.state == "running") {
            return ToolResult::Error("an ab_test of '" + job.setting + "' is already running; "
                                     "follow it with ab_test_status or end it with ab_test_stop");
        }
    }
    if (const auto a = Activity::Get().Snapshot();
        a.route_state == "running" && !a.route.starts_with("ab_test") &&
        steady_clock::now() - a.route_changed < minutes{10}) {
        return ToolResult::Error("route '" + a.route + "' is running; wait for it (route_status) "
                                 "or end it with route_stop before starting ab_test");
    }

    Settings::BasicSetting* setting = FindSetting(cfg.setting);
    if (setting == nullptr) {
        return ToolResult::Error("unknown or hidden setting '" + cfg.setting +
                                 "'; settings_list shows the names");
    }
    // Reject bad values before changing anything.
    const std::string original_raw = setting->ToString();
    for (const auto& value : cfg.values) {
        const std::string error = SetFromString(*setting, value);
        setting->LoadString(original_raw);
        if (!error.empty()) {
            return ToolResult::Error(error + "; settings_get shows the type, range and current "
                                             "value");
        }
    }

    cfg.not_live = !setting->RuntimeModifiable();
    cfg.reboot = reboot_mode == "always" || (reboot_mode == "auto" && cfg.not_live);
    json notes = json::array();
    if (cfg.reboot) {
        if (setting->Switchable() && !setting->UsingGlobal()) {
            return ToolResult::Error(
                "'" + cfg.setting + "' has a per-game override, which a reboot reloads from the "
                "game's configuration, so the test values would not survive it; remove the "
                "override in the game's properties or pass reboot:\"never\"");
        }
        std::string error, note;
        cfg.game_path = RunningGamePath(ctx, error, note);
        if (cfg.game_path.empty()) {
            return ToolResult::Error(error);
        }
        if (!note.empty()) {
            notes.push_back(note);
        }
    } else if (!cfg.boot_route.empty()) {
        notes.push_back("boot_route is only used after a reboot; this test does not reboot");
    }

    const u64 since_seq = ctx.timeline ? ctx.timeline->LastSeq() : 0;
    std::unique_lock lock{job.mutex};
    // Checked under the lock StopAbTestWork takes, so no test starts after shutdown began.
    if (ServerStopping()) {
        return ToolResult::Error("the MCP server is stopping");
    }
    if (job.thread.joinable()) {
        // Not running (checked above), so the old thread has only its return left.
        job.thread.join();
    }
    job.state = "running";
    job.background = background;
    job.session = ctx.session_id;
    job.setting = cfg.setting;
    job.values = cfg.values;
    job.index = 0;
    job.phase = "starting";
    job.rows = json::array();
    job.result = nullptr;
    job.started = steady_clock::now();

    if (background) {
        ToolContext copy = ctx;
        job.thread = std::jthread([copy, cfg](std::stop_token stop) {
            json result;
            try {
                result = RunAbTest(copy, cfg, stop);
            } catch (const std::exception& e) {
                LOG_ERROR(Debug_MCP, "ab_test {} failed: {}", cfg.setting, e.what());
                result = json{{"state", "failed"}, {"aborted", e.what()}};
            } catch (...) {
                result = json{{"state", "failed"}, {"aborted", "internal error"}};
            }
            FinishJob(result);
        });
        job.stop_source = job.thread.get_stop_source();
        return ToolResult::Structured(
            json{{"started", cfg.setting},
                 {"values", cfg.values},
                 {"reboot", cfg.reboot},
                 {"notes", std::move(notes)},
                 {"since_seq", since_seq},
                 {"hint", "Progress: ab_test_status, the status bar, or wait_for {source_prefix: "
                          "'ab_test', contains: 'best'}. ab_test_stop ends it early."}});
    }

    job.stop_source = std::stop_source{};
    const std::stop_token stop = job.stop_source.get_token();
    lock.unlock();
    json result = RunAbTest(ctx, cfg, stop);
    FinishJob(result);
    if (!notes.empty()) {
        result["notes"] = std::move(notes);
    }
    result["since_seq"] = since_seq;
    ToolResult r = ToolResult::Structured(result);
    r.is_error = result.value("state", "failed") != "done";
    return r;
}

/// ab_test_status tool: state, current value and phase, rows so far and, once finished, the
/// full result of the current or last ab_test.
ToolResult AbTestStatus(ToolContext&, const json&) {
    auto& job = Job();
    std::scoped_lock lock{job.mutex};
    if (job.state.empty()) {
        return ToolResult::Structured(
            json{{"state", "idle"}, {"hint", "No ab_test has run since the emulator started."}});
    }
    const bool running = job.state == "running";
    json out{{"state", running && job.stop_source.stop_requested() ? "stopping" : job.state},
             {"setting", job.setting},
             {"values", job.values},
             {"background", job.background},
             {"elapsed_ms", duration_cast<milliseconds>(steady_clock::now() - job.started).count()},
             {"table", job.rows}};
    if (running) {
        out["value_index"] = job.index;
        if (job.index < job.values.size()) {
            out["value"] = job.values[job.index];
        }
        out["phase"] = job.phase;
    } else {
        out["result"] = job.result;
    }
    return ToolResult::Structured(std::move(out));
}

/// ab_test_stop tool: asks the running test to end. It still restores the original value (and
/// reboots back when needed) before it finishes, so this returns at once.
ToolResult AbTestStop(ToolContext&, const json&) {
    auto& job = Job();
    std::scoped_lock lock{job.mutex};
    if (job.state != "running") {
        return ToolResult::Text("no ab_test is running");
    }
    job.stop_source.request_stop();
    return ToolResult::Text("stop requested: the test ends after the current wait or measurement, "
                            "restores the original value (rebooting back if it rebooted) and "
                            "then reports state 'stopped' in ab_test_status");
}

} // namespace

void StopSessionAbTest(const std::string& session) {
    // Not joined: the input it sends stops at once through the stop token, but restoring the
    // setting may reboot the game, which can take a while.
    auto& job = Job();
    std::scoped_lock lock{job.mutex};
    if (job.state == "running" && job.session == session) {
        job.stop_source.request_stop();
    }
}

void StopAbTestWork() {
    auto& job = Job();
    std::jthread thread;
    {
        std::scoped_lock lock{job.mutex};
        job.stop_source.request_stop();
        thread = std::move(job.thread);
    }
    if (thread.joinable()) {
        thread.join();
    }
}

void RegisterAbTestTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "ab_test",
        .title = "A/B test a setting",
        .description =
            "How a setting affects performance, in one call: for each of `values` (2-6), set it "
            "(as settings_set does), reboot the game if needed (reboot: auto = only when the "
            "setting is not runtime-modifiable, always, never; after a reboot boot_route gets "
            "back to the measuring point), then run `route` (a saved benchmark route with sample "
            "steps) or a perf_sample of sample_seconds (default 8). Restores the original value "
            "at the end, also on failure. Returns a table (fps_avg, one_percent_low, "
            "frame_ms_median, frame_ms_p95, speed_pct, route_ok, notes per value), the best value "
            "by fps_avg and the raw samples. background:true returns at once: follow it with "
            "ab_test_status, end it with ab_test_stop.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"setting", {{"type", "string"},
                            {"minLength", 1},
                            {"description", "Setting name from settings_list, e.g. "
                                            "resolution_setup."}}},
               {"values", {{"type", "array"},
                           {"items", {{"type", "string"}}},
                           {"minItems", 2},
                           {"maxItems", 6},
                           {"description", "Values to compare; enums take names or indices."}}},
               {"route", {{"type", "string"},
                          {"description", "Saved route run per value; its sample steps are "
                                          "the measurements."}}},
               {"sample_seconds", {{"type", "number"}, {"minimum", 2}, {"maximum", 60}}},
               {"boot_route", {{"type", "string"},
                               {"description", "Saved route from a fresh boot to the measuring "
                                               "point, run after each reboot."}}},
               {"reboot", {{"type", "string"}, {"enum", {"auto", "always", "never"}}}},
               {"background", {{"type", "boolean"}}}}},
             {"required", {"setting", "values"}},
             {"additionalProperties", false}},
        .annotations = {.destructive = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = AbTest,
    });
    registry.Add(Tool{
        .name = "ab_test_status",
        .title = "A/B test status",
        .description = "Progress of the running ab_test (value, phase, rows so far) or the full "
                       "result of the last one.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = AbTestStatus,
    });
    registry.Add(Tool{
        .name = "ab_test_stop",
        .title = "Stop the A/B test",
        .description = "End the running ab_test early. It still restores the original value "
                       "(rebooting back if it rebooted) before reporting state 'stopped'.",
        .requires_control = true,
        .handler = AbTestStop,
    });
}

} // namespace Mcp
