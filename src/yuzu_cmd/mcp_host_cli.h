// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "mcp/host.h"

namespace Core {
class System;
}

/// eden-cli implementation of the MCP host, for unattended/headless runs (`eden-cli --mcp ...`).
/// Actions that must run on the SDL main thread are queued and drained by SDL_AppIterate
/// (DrainMainThread). The game is the one given on the command line: no game list or boot.
class CliMcpHost final : public Mcp::Host {
public:
    /// system and input must outlive this host.
    CliMcpHost(Core::System& system, InputCommon::InputSubsystem& input);

    std::string FrontendName() const override;
    /// Returns the stats last sampled by DrainMainThread. Does not block on the main thread.
    Mcp::PerfSnapshot GetPerfSnapshot() override;
    InputCommon::InputSubsystem* GetInputSubsystem() override;
    /// Always fails: eden-cli cannot boot another game.
    std::string BootGame(const std::string& path) override;
    /// Pauses the system on the main thread (blocking, up to 30 seconds).
    std::string Pause() override;
    /// Resumes the system on the main thread (blocking, up to 30 seconds).
    std::string Resume() override;
    /// Sets the quit flag and returns at once: there is no idle state, so the app exits.
    std::string Stop() override;
    /// Sets the quit flag and returns at once; SDL_AppIterate ends the app.
    std::string Quit() override;
    /// Applies Settings::values on the main thread (blocking). No-op if the system is off.
    std::string ApplySettings() override;
    /// Runs fn against the TAS driver on the main thread (blocking). Resets player 1's system
    /// buttons first when a game is running.
    std::string WithTas(bool require_game,
                        std::function<void(InputCommon::TasInput::Tas&)> fn) override;
    /// Captures a frame via CaptureFrame, converts it to RGBA and encodes it as PNG with stb.
    std::optional<std::vector<u8>> CaptureScreenshotPng(u32 max_width,
                                                        std::chrono::milliseconds timeout,
                                                        std::string& error) override;
    /**
     * Requests a 16:9 screenshot from the renderer and waits for it on the calling thread.
     * Needs a window that presents frames; it times out with a hidden or null window.
     * @param width Output width, clamped to 160..3840; the height is width * 9 / 16.
     * @param error Set to a reason when nullopt is returned (no game, capture pending, timeout).
     */
    std::optional<Mcp::Frame> CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                           std::string& error) override;

    /// Called by SDL_AppIterate on the main thread: runs queued actions, refreshes perf.
    void DrainMainThread();
    /// Set by Stop/Quit: SDL_AppIterate ends the app.
    [[nodiscard]] bool QuitRequested() const {
        return quit_requested.load();
    }

private:
    /// Runs fn on the SDL main thread and waits up to 30 s for it. On timeout it returns an error,
    /// but fn stays queued and may still run later.
    std::string RunOnMain(std::function<std::string()> fn);

    Core::System& system;
    InputCommon::InputSubsystem& input;
    /// Guards queue: pushed by MCP worker threads, swapped out by DrainMainThread.
    std::mutex queue_mutex;
    std::vector<std::function<void()>> queue;
    std::mutex perf_mutex;
    Mcp::PerfSnapshot perf;
    /// When DrainMainThread last sampled perf (it samples at most every 500 ms).
    std::chrono::steady_clock::time_point last_perf;
    std::atomic<bool> quit_requested{false};
};
