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
    CliMcpHost(Core::System& system, InputCommon::InputSubsystem& input);

    std::string FrontendName() const override;
    Mcp::PerfSnapshot GetPerfSnapshot() override;
    InputCommon::InputSubsystem* GetInputSubsystem() override;
    std::string BootGame(const std::string& path) override;
    std::string Pause() override;
    std::string Resume() override;
    std::string Stop() override;
    std::string Quit() override;
    std::string ApplySettings() override;
    std::string WithTas(bool require_game,
                        std::function<void(InputCommon::TasInput::Tas&)> fn) override;
    std::optional<std::vector<u8>> CaptureScreenshotPng(u32 max_width,
                                                        std::chrono::milliseconds timeout,
                                                        std::string& error) override;
    std::optional<Mcp::Frame> CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                           std::string& error) override;

    /// Called by SDL_AppIterate on the main thread: runs queued actions, refreshes perf.
    void DrainMainThread();
    /// Set by Stop/Quit: SDL_AppIterate ends the app.
    [[nodiscard]] bool QuitRequested() const {
        return quit_requested.load();
    }

private:
    /// Runs fn on the SDL main thread and waits for it (errors if the app stops meanwhile).
    std::string RunOnMain(std::function<std::string()> fn);

    Core::System& system;
    InputCommon::InputSubsystem& input;
    std::mutex queue_mutex;
    std::vector<std::function<void()>> queue;
    std::mutex perf_mutex;
    Mcp::PerfSnapshot perf;
    std::chrono::steady_clock::time_point last_perf;
    std::atomic<bool> quit_requested{false};
};
