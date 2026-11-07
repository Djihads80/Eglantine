// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <optional>

#include <QImage>

#include "mcp/host.h"

class MainWindow;

namespace Core {
struct PerfStatsResults;
}
namespace VideoCore {
class ShaderNotify;
}

/// Qt implementation of the MCP frontend host. Called from MCP worker threads; everything that
/// touches widgets or the emulation thread runs on the GUI thread via blocking queued calls.
class QtMcpHost final : public Mcp::Host {
public:
    /// The window must outlive this host (MainWindow owns both the host and the MCP server).
    explicit QtMcpHost(MainWindow& window);

    std::string FrontendName() const override;
    /// Returns the last stats copied by OnStatsUpdated, marked invalid when no game is running.
    /// Does not block on the GUI thread.
    Mcp::PerfSnapshot GetPerfSnapshot() override;
    InputCommon::InputSubsystem* GetInputSubsystem() override;
    /// Walks the game list model on the GUI thread (blocking) and returns one entry per unique
    /// path, or nullopt if the game list widget does not exist.
    std::optional<std::vector<Mcp::GameEntry>> ListGames() override;
    /// Boots the game on the GUI thread (blocking). Fails if a game is already running.
    std::string BootGame(const std::string& path) override;
    /// Pauses emulation on the GUI thread (blocking). Fails if no game is running.
    std::string Pause() override;
    /// Resumes emulation on the GUI thread (blocking). Fails if no game is running.
    std::string Resume() override;
    /// Shuts the game down on the GUI thread (blocking), without a confirmation prompt.
    std::string Stop() override;
    /// Queues a stop and window close on the GUI thread and returns at once, so the reply can be
    /// sent before the MCP server is destroyed.
    std::string Quit() override;
    /// Applies Settings::values to the running system on the GUI thread (blocking). No-op when
    /// no game is running.
    std::string ApplySettings() override;
    /// Switches docked/handheld mode on the GUI thread (blocking), like the status bar button but
    /// without its warning box. Docking with handheld connected moves player 1 to Pro Controller.
    std::string SetDocked(bool docked) override;
    /// Runs fn against the TAS driver on the GUI thread (blocking), then refreshes the TAS status
    /// label. Resets player 1's system buttons first when a game is running.
    std::string WithTas(bool require_game,
                        std::function<void(InputCommon::TasInput::Tas&)> fn) override;
    /// Captures a frame via CaptureImage and encodes it as PNG on the calling thread.
    std::optional<std::vector<u8>> CaptureScreenshotPng(u32 max_width,
                                                        std::chrono::milliseconds timeout,
                                                        std::string& error) override;
    /// Captures a frame via CaptureImage and copies it into tightly packed BGRA rows.
    std::optional<Mcp::Frame> CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                           std::string& error) override;

    /// Called on the GUI thread from MainWindow::statsUpdated.
    void OnStatsUpdated(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);

private:
    /**
     * Requests a 16:9 screenshot from the renderer and waits for it on the calling thread.
     * @param max_width Output width, clamped to 160..3840; the height is width * 9 / 16.
     * @param timeout How long to wait for the renderer to deliver a frame.
     * @param error Set to a reason when nullopt is returned (no game, capture pending, timeout).
     * @return The image in Format_RGB32, or nullopt on failure.
     */
    std::optional<QImage> CaptureImage(u32 max_width, std::chrono::milliseconds timeout,
                                       std::string& error);

    MainWindow& window;
    /// Guards perf, which is written on the GUI thread and read from MCP worker threads.
    std::mutex perf_mutex;
    Mcp::PerfSnapshot perf;
};
