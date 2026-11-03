// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU profiling as data: toggles the NX2 GPU-thread profiler at runtime and returns each
// thread's last report (zones, counters) as JSON.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <fmt/format.h>

#include "mcp/context.h"
#include "mcp/timeline.h"
#include "mcp/host.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"
#include "video_core/nx2_profile.h"

namespace Mcp {

namespace {

namespace Profile = VideoCommon::Nx2Profile;

/// One GPU thread's profiler report as JSON: zones sorted by cost (only the `top` costliest
/// when non-zero), non-zero counters, and the report's age relative to `now` (ns).
json SnapshotJson(const Profile::Snapshot& snap, u64 now, size_t top) {
    std::vector<Profile::Snapshot::ZoneStat> zones = snap.zones;
    std::sort(zones.begin(), zones.end(),
              [](const auto& a, const auto& b) { return a.ms > b.ms; });
    if (top != 0 && zones.size() > top) {
        zones.resize(top);
    }
    json zone_list = json::array();
    for (const auto& z : zones) {
        zone_list.push_back({{"zone", z.name}, {"ms_per_s", z.ms}, {"calls_per_s", z.calls}});
    }
    json counters = json::object();
    for (const auto& [name, value] : snap.counters) {
        if (value != 0.0) {
            counters[name] = value;
        }
    }
    // Split GPU thread: how often the front waited for the back (the "split drain" zone).
    double total_drains = 0.0;
    for (const auto& z : snap.zones) {
        if (std::string_view{z.name} == "split drain") {
            total_drains = z.calls;
        }
    }
    return json{{"thread", snap.thread},
                {"age_ms", static_cast<double>(now - snap.when_ns) / 1e6},
                {"window_ms", static_cast<double>(snap.window_ns) / 1e6},
                {"zones", std::move(zone_list)},
                {"counters_per_s", std::move(counters)},
                {"split_drains_total_per_s", total_drains}};
}

/// gpu_profile tool: applies enable/window_ms, then returns every GPU thread's last report.
/// With `fresh`, polls for up to 2 windows + 3 s until each report covers only time after the
/// call. Takes the profiler's snapshot mutex while reading.
ToolResult GpuProfile(ToolContext& ctx, const json& args) {
    using namespace std::chrono;
    const bool was_enabled = Profile::Enabled();
    if (args.contains("window_ms")) {
        Profile::g_window_ns.store(args["window_ms"].get<u64>() * 1'000'000ULL,
                                   std::memory_order_relaxed);
    }
    if (args.contains("enable") && args["enable"].get<bool>() != was_enabled) {
        Profile::SetEnabled(args["enable"].get<bool>());
    }
    const bool enabled = Profile::Enabled();
    const u64 window_ms = Profile::g_window_ns.load(std::memory_order_relaxed) / 1'000'000ULL;

    // Fresh data: wait for a report that covers only time after this call.
    const bool fresh = args.value("fresh", enabled && !was_enabled);
    const u64 called = Profile::Now();
    if (fresh && enabled && ctx.GameRunning()) {
        const auto deadline = steady_clock::now() + milliseconds{2 * window_ms + 3000};
        while (steady_clock::now() < deadline) {
            // The front ("GPU thread") always reports; the split's back thread too when active.
            size_t newer = 0;
            size_t total = 0;
            bool front_fresh = false;
            {
                std::scoped_lock lock{Profile::SnapshotMutex()};
                total = Profile::Snapshots().size();
                for (const auto& [name, snap] : Profile::Snapshots()) {
                    const bool is_fresh = snap.when_ns - snap.window_ns >= called;
                    newer += is_fresh ? 1 : 0;
                    front_fresh |= is_fresh && name == "GPU thread";
                }
            }
            if (front_fresh && newer >= total) {
                break;
            }
            std::this_thread::sleep_for(milliseconds{50});
        }
    }

    const size_t top = args.value("top_zones", size_t{0});
    json threads = json::array();
    const u64 now = Profile::Now();
    {
        std::scoped_lock lock{Profile::SnapshotMutex()};
        for (const auto& [name, snap] : Profile::Snapshots()) {
            threads.push_back(SnapshotJson(snap, now, top));
        }
    }
    json out{{"enabled", enabled}, {"window_ms", window_ms}, {"threads", std::move(threads)}};
    if (json perf = PerfJson(ctx); !perf.is_null()) {
        out["perf"] = std::move(perf);
    }
    if (!enabled) {
        out["hint"] = "Profiler is off; call with enable:true (costs a few % GPU-thread time).";
    } else if (out["threads"].empty()) {
        out["hint"] = "No report yet: one arrives every window_ms while a game renders.";
    }
    out["legend"] = "zones: exclusive ms per second of wall time per GPU thread ('GPU thread' = "
                    "front/parsing, 'GPU back thread' = Vulkan recording). 'split drain' on the "
                    "front = waiting for the back; 'split wait' on the back = idle.";
    return ToolResult::Structured(std::move(out));
}

#ifdef _WIN32
/// CPU time (100 ns units) and name of every thread of this process.
std::map<u32, std::pair<std::string, u64>> SampleHostThreads() {
    std::map<u32, std::pair<std::string, u64>> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return out;
    }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        FILETIME created, exited, kernel, user;
        u64 cpu = 0;
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            cpu = (u64{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
                  (u64{user.dwHighDateTime} << 32 | user.dwLowDateTime);
        }
        std::string name;
        PWSTR wide = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                name.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), len, nullptr, nullptr);
            }
            LocalFree(wide);
        }
        CloseHandle(h);
        out[te.th32ThreadID] = {name.empty() ? fmt::format("thread {}", te.th32ThreadID) : name, cpu};
    }
    CloseHandle(snap);
    return out;
}
#endif

/// Frame pacing, GPU profile, host thread CPU and an optional baseline check over one window:
/// the repeatable "before/after" measurement for performance work.
/// Blocks the calling thread for `seconds` (ends early on server stop), then up to 3 s more for
/// the GPU report. Restores the profiler's previous enable state and window afterwards.
ToolResult PerfSample(ToolContext& ctx, const json& args) {
    using namespace std::chrono;
    const int seconds = args.value("seconds", 10);
    const bool profile = args.value("profile", true);
    const bool was_enabled = Profile::Enabled();
    const u64 old_window = Profile::g_window_ns.load(std::memory_order_relaxed);
    if (profile) {
        // One report covering exactly this window (toggling restarts every thread's totals).
        Profile::g_window_ns.store(static_cast<u64>(seconds) * 1'000'000'000ULL,
                                   std::memory_order_relaxed);
        Profile::SetEnabled(false);
        Profile::SetEnabled(true);
    }
#ifdef _WIN32
    const auto threads_before = SampleHostThreads();
#endif
    const auto started = steady_clock::now();
    const u64 profile_from = Profile::Now();
    std::vector<double> intervals_ms;
    u64 last_frame = PresentedFrames();
    auto last_time = steady_clock::now();
    const u64 first_frame = last_frame;
    while (steady_clock::now() - started < std::chrono::seconds{seconds} && !ServerStopping()) {
        std::this_thread::sleep_for(milliseconds{1});
        const u64 frame = PresentedFrames();
        if (frame != last_frame) {
            const auto now = steady_clock::now();
            // Several presents within one poll share the time between them.
            const double ms = duration<double, std::milli>(now - last_time).count() /
                              static_cast<double>(frame - last_frame);
            for (u64 i = last_frame; i < frame; ++i) {
                intervals_ms.push_back(ms);
            }
            last_frame = frame;
            last_time = now;
        }
    }
    const double wall_s = duration<double>(steady_clock::now() - started).count();
#ifdef _WIN32
    const auto threads_after = SampleHostThreads();
#endif

    json out;
    out["seconds"] = std::round(wall_s * 10) / 10;
    out["frames"] = last_frame - first_frame;
    if (!intervals_ms.empty()) {
        std::vector<double> sorted = intervals_ms;
        std::sort(sorted.begin(), sorted.end());
        const auto pct = [&](double p) {
            return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))];
        };
        const auto r1 = [](double v) { return std::round(v * 10) / 10; };
        out["fps"] = {{"average", r1(static_cast<double>(sorted.size()) / wall_s)},
                      {"frame_ms_median", r1(pct(0.5))},
                      {"frame_ms_p95", r1(pct(0.95))},
                      {"frame_ms_p99", r1(pct(0.99))},
                      {"frame_ms_worst", r1(sorted.back())},
                      {"one_percent_low_fps", r1(1000.0 / pct(0.99))}};
    } else {
        out["fps"] = {{"average", 0}, {"note", "no frames presented (paused or loading?)"}};
    }
    if (json perf = PerfJson(ctx); !perf.is_null()) {
        out["emulation_speed_pct"] = perf.value("speed_pct", 0.0);
    }

#ifdef _WIN32
    json threads = json::array();
    std::vector<std::pair<double, json>> rows;
    for (const auto& [tid, info] : threads_after) {
        const auto it = threads_before.find(tid);
        const u64 before = it == threads_before.end() ? 0 : it->second.second;
        const double pct = static_cast<double>(info.second - before) / (wall_s * 1e7) * 100.0;
        if (pct >= 1.0) {
            rows.emplace_back(pct, json{{"thread", info.first}, {"cpu_pct", std::round(pct)}});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 12); ++i) {
        threads.push_back(std::move(rows[i].second));
    }
    out["host_threads"] = std::move(threads);
#endif

    if (profile) {
        const auto deadline = steady_clock::now() + milliseconds{3000};
        json gpu = json::array();
        while (steady_clock::now() < deadline) {
            gpu = json::array();
            bool front = false;
            {
                std::scoped_lock lock{Profile::SnapshotMutex()};
                for (const auto& [name, snap] : Profile::Snapshots()) {
                    if (snap.when_ns >= profile_from) {
                        gpu.push_back(SnapshotJson(snap, Profile::Now(), 6));
                        front |= name == "GPU thread";
                    }
                }
            }
            if (front) {
                break;
            }
            std::this_thread::sleep_for(milliseconds{100});
        }
        out["gpu_profile"] = std::move(gpu);
        Profile::g_window_ns.store(old_window, std::memory_order_relaxed);
        Profile::SetEnabled(was_enabled);
    }

    ToolResult result;
    if (args.contains("baseline")) {
        std::string error;
        std::vector<u8> png;
        const json check = BaselineCheck(ctx, args["baseline"].get<std::string>(),
                                         args.value("baseline_threshold", 0.9), error, &png);
        out["baseline"] = check.is_null() ? json{{"error", error}} : check;
        if (!png.empty()) {
            result.AddImage(png, "image/png");
        }
    } else if (args.value("screenshot", true) && ctx.host) {
        std::string error;
        if (const auto png = ctx.host->CaptureScreenshotPng(480, milliseconds{2000}, error)) {
            result.AddImage(*png, "image/png");
        }
    }
    if (args.contains("label")) {
        out["label"] = args["label"];
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::Mark, 0, "perf",
                               fmt::format("perf_sample {}: {} fps avg", args["label"].get<std::string>(),
                                           out["fps"].value("average", 0.0)));
        }
    }
    ToolResult structured = ToolResult::Structured(std::move(out));
    for (auto& block : result.content) {
        structured.content.push_back(std::move(block));
    }
    return structured;
}

} // namespace

/// Runs perf_sample and returns its structured result, or an {"error"} object.
json RunPerfSample(ToolContext& ctx, const json& args) {
    ToolResult r = PerfSample(ctx, args);
    return r.structured ? *r.structured : json{{"error", "no data"}};
}

namespace {

/// baseline_save tool: wraps BaselineSave, turning its error into a tool error.
ToolResult BaselineSaveTool(ToolContext& ctx, const json& args) {
    std::string error;
    const json r = BaselineSave(ctx, args.at("name").get<std::string>(), error);
    return r.is_null() ? ToolResult::Error(error) : ToolResult::Structured(r);
}

/// baseline_check tool: the BaselineCheck result plus the current frame as a PNG.
ToolResult BaselineCheckTool(ToolContext& ctx, const json& args) {
    std::string error;
    std::vector<u8> png;
    const json r = BaselineCheck(ctx, args.at("name").get<std::string>(),
                                 args.value("threshold", 0.9), error, &png);
    if (r.is_null()) {
        return ToolResult::Error(error);
    }
    ToolResult result = ToolResult::Structured(r);
    if (!png.empty()) {
        result.AddImage(png, "image/png");
    }
    return result;
}

} // namespace

/// Registers perf_sample, baseline_save, baseline_check and gpu_profile.
void RegisterPerfTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "perf_sample",
        .title = "Measure performance",
        .description =
            "Measure for `seconds` (default 10): frame pacing (average fps, median/p95/p99/worst "
            "frame time, 1% low), a GPU-thread profile covering exactly that window, host CPU per "
            "emulator thread, and either a baseline check (baseline: name -> similarity score + "
            "current image) or a screenshot. The before/after measurement for performance work: "
            "run it at the same route checkpoint before and after a change.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"seconds", {{"type", "integer"}, {"minimum", 2}, {"maximum", 60}}},
                           {"profile", {{"type", "boolean"}}},
                           {"baseline", {{"type", "string"}}},
                           {"baseline_threshold", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                           {"screenshot", {{"type", "boolean"}}},
                           {"label", {{"type", "string"}, {"maxLength", 100}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = PerfSample,
    });
    registry.Add(Tool{
        .name = "baseline_save",
        .title = "Save a visual baseline",
        .description = "Save the current frame as a named known-good image for this game (PNG + "
                       "fingerprint under <config>/mcp/baselines/), for later baseline_check / "
                       "perf_sample baseline comparisons after code changes.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}}},
                         {"required", {"name"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .handler = BaselineSaveTool,
    });
    registry.Add(Tool{
        .name = "baseline_check",
        .title = "Compare with a visual baseline",
        .description = "Score the current frame against a saved baseline (1.0 = same picture; "
                       "below threshold, default 0.9, means the rendering changed) and return the "
                       "current image; baseline_png is the stored known-good image.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}}},
                           {"threshold", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
                         {"required", {"name"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = BaselineCheckTool,
    });
    registry.Add(Tool{
        .name = "gpu_profile",
        .title = "GPU thread profile",
        .description =
            "Where the GPU threads spend their time, as data: per thread, exclusive ms/s and "
            "calls/s per zone (draw, macro, update buffers, split drain...), counters (register "
            "writes, downloads, semaphore acquires) and split drains, plus fps. enable turns the "
            "profiler on/off live; window_ms sets the report window (default 10000); fresh waits "
            "for a report covering only time after the call (default when just enabled).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"enable", {{"type", "boolean"}}},
                           {"window_ms", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 60000}}},
                           {"fresh", {{"type", "boolean"}}},
                           {"top_zones", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64},
                                          {"description", "Only the N costliest zones (0 = all)."}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .handler = GpuProfile,
    });
}

} // namespace Mcp
