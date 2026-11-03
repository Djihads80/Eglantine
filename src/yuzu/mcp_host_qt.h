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
    explicit QtMcpHost(MainWindow& window);

    std::string FrontendName() const override;
    Mcp::PerfSnapshot GetPerfSnapshot() override;
    InputCommon::InputSubsystem* GetInputSubsystem() override;
    std::optional<std::vector<Mcp::GameEntry>> ListGames() override;
    std::string BootGame(const std::string& path) override;
    std::string Pause() override;
    std::string Resume() override;
    std::string Stop() override;
    std::string Quit() override;
    std::string ApplySettings() override;
    std::string SetDocked(bool docked) override;
    std::string WithTas(bool require_game,
                        std::function<void(InputCommon::TasInput::Tas&)> fn) override;
    std::optional<std::vector<u8>> CaptureScreenshotPng(u32 max_width,
                                                        std::chrono::milliseconds timeout,
                                                        std::string& error) override;
    std::optional<Mcp::Frame> CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                           std::string& error) override;

    /// Called on the GUI thread from MainWindow::statsUpdated.
    void OnStatsUpdated(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);

private:
    std::optional<QImage> CaptureImage(u32 max_width, std::chrono::milliseconds timeout,
                                       std::string& error);

    MainWindow& window;
    std::mutex perf_mutex;
    Mcp::PerfSnapshot perf;
};
