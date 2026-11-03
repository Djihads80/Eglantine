// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Pieces shared between tool files (composite tools such as `act` reuse the single-purpose ones).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mcp/timeline.h"
#include "mcp/tool.h"

namespace Mcp {

struct ToolContext;

// input_tools.cpp
/// Schema of the `player` argument (1-8 = P1-P8, 9 = handheld).
json PlayerSchema();
/// Schema of one input_sequence-style step list.
json InputStepsSchema();
/// npad index from the `player` argument (1-8 = P1-P8, 9 = handheld).
size_t NpadIndex(const json& args);
/// Runs input_sequence steps on the virtual gamepad. Returns an error message, or "" on success.
std::string RunInputSteps(ToolContext& ctx, size_t npad, const json& steps);
/// Buttons and sticks as the game sees them.
json ControllerStateJson(ToolContext& ctx, size_t npad);

// core_tools.cpp
/// One timeline event as JSON (seq, t_ms, frame, kind, source, msg; level for logs).
json EventJson(const Event& event);
/// Filter properties shared by timeline, wait_for and act.
json FilterProperties();
/// Builds a timeline query from FilterProperties-style arguments. Unknown kinds and levels are
/// ignored.
Timeline::Query ParseQuery(const json& args);
/// Frontend fps, emulation speed, frame time and shader builds, or null when there is no host
/// or no valid sample yet.
json PerfJson(ToolContext& ctx);

// game_state_tools.cpp
/// {screen, scores} for the current frame, or null (error set).
json GameScreenState(ToolContext& ctx, std::string& error);
/// Waits until the screen is one of `labels`; returns the label or "" on timeout.
std::string GameWaitScreen(ToolContext& ctx, const json& labels, std::chrono::milliseconds timeout,
                           json& state);
/// All saved probes of the running game with current values.
json GameProbes(ToolContext& ctx);
// perf_tools.cpp
/// perf_sample's measurement as JSON (for route steps that measure at a checkpoint).
json RunPerfSample(ToolContext& ctx, const json& args);

/// Saves the current frame as a named per-game visual baseline (fingerprint + PNG).
json BaselineSave(ToolContext& ctx, const std::string& name, std::string& error);
/// Scores the current frame against a baseline: {score, pass, baseline_png}; optional PNG out.
json BaselineCheck(ToolContext& ctx, const std::string& name, double threshold, std::string& error,
                   std::vector<u8>* current_png);
/// Stops a background route and a recording (server shutdown: they use the host).
void StopGameStateWork();
/// Ends memory watches and their sampling thread (server shutdown).
void StopMemoryWork();
/// PNG of the capture layer (works paused/hidden), scaled to max_width.
std::optional<std::vector<u8>> RawCapturePng(ToolContext& ctx, u32 max_width, std::string& error);

// trace_tools.cpp: core observers feeding the timeline (installed while the server runs).
/// Installs the unimplemented-command and crash hooks that post timeline events.
void InstallTraceHooks();
/// Clears all trace hooks, including the IPC and SVC ones the trace tools may have enabled.
void RemoveTraceHooks();

/// Set while the server shuts down: long waits in tools check it and return early.
inline std::atomic<bool> g_server_stopping{false};
/// Whether the server is shutting down (reads g_server_stopping).
[[nodiscard]] inline bool ServerStopping() {
    return g_server_stopping.load(std::memory_order_relaxed);
}
/// Sleeps up to `duration`, returning early (false) when the server is stopping.
inline bool SleepUnlessStopping(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        if (ServerStopping()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::milliseconds>(std::chrono::milliseconds{50},
                                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    until - std::chrono::steady_clock::now()) +
                                                    std::chrono::milliseconds{1}));
    }
    return true;
}

/// Guest frames presented so far.
u64 PresentedFrames();
/// Waits until `frames` more frames are presented. Returns the number actually presented
/// (fewer on timeout, e.g. when paused or loading).
u64 WaitFrames(u64 frames, std::chrono::milliseconds timeout);

} // namespace Mcp
