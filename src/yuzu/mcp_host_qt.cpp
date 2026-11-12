// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <future>
#include <memory>

#include <QBuffer>
#include <QImage>
#include <QMetaObject>
#include <QStandardItemModel>

#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/qt_common.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/debugger/controller.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"
#include "yuzu/mcp_host_qt.h"

namespace {

/// Runs fn on the window's (GUI) thread and waits for it. Must not be called from the GUI thread.
template <typename Fn>
auto RunOnGuiThread(QObject* context, Fn&& fn) {
    using R = decltype(fn());
    if constexpr (std::is_void_v<R>) {
        QMetaObject::invokeMethod(context, std::forward<Fn>(fn), Qt::BlockingQueuedConnection);
    } else {
        R result{};
        QMetaObject::invokeMethod(
            context, [&] { result = fn(); }, Qt::BlockingQueuedConnection);
        return result;
    }
}

/// Recursively appends every item under item that has a non-empty full path (folders and
/// favorites included, so the result may hold duplicates).
void CollectGames(const QStandardItem* item, std::vector<Mcp::GameEntry>& out) {
    if (item == nullptr) {
        return;
    }
    const QVariant path = item->data(GameListItemPath::FullPathRole);
    if (path.isValid() && !path.toString().isEmpty()) {
        out.push_back(Mcp::GameEntry{
            .path = path.toString().toStdString(),
            .title = item->data(GameListItemPath::TitleRole).toString().toStdString(),
            .program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong(),
        });
    }
    for (int row = 0; row < item->rowCount(); ++row) {
        CollectGames(item->child(row, 0), out);
    }
}

} // namespace

QtMcpHost::QtMcpHost(MainWindow& window_) : window{window_} {}

std::string QtMcpHost::FrontendName() const {
    return "eglantine (Qt)";
}

void QtMcpHost::OnStatsUpdated(const Core::PerfStatsResults& results,
                               const VideoCore::ShaderNotify& shaders) {
    std::scoped_lock lock{perf_mutex};
    perf.valid = true;
    perf.game_fps = results.average_game_fps;
    perf.emulation_speed = results.emulation_speed;
    perf.frametime_ms = results.frametime * 1000.0;
    perf.shaders_building = static_cast<u32>(shaders.NumBuildingNoReport());
}

/// The last stats update, marked invalid when no game is running.
Mcp::PerfSnapshot QtMcpHost::GetPerfSnapshot() {
    std::scoped_lock lock{perf_mutex};
    Mcp::PerfSnapshot snapshot = perf;
    if (!window.emulation_running) {
        snapshot.valid = false;
    }
    return snapshot;
}

InputCommon::InputSubsystem* QtMcpHost::GetInputSubsystem() {
    return window.input_subsystem.get();
}

/// The game list's entries, one per path, read on the GUI thread.
std::optional<std::vector<Mcp::GameEntry>> QtMcpHost::ListGames() {
    return RunOnGuiThread(&window, [this]() -> std::optional<std::vector<Mcp::GameEntry>> {
        if (window.game_list == nullptr) {
            return std::nullopt;
        }
        std::vector<Mcp::GameEntry> games;
        const QStandardItemModel* model = window.game_list->GetModel();
        CollectGames(model->invisibleRootItem(), games);
        // Favorites duplicate entries; keep the first of each path.
        std::vector<Mcp::GameEntry> unique;
        for (auto& game : games) {
            if (std::none_of(unique.begin(), unique.end(),
                             [&](const auto& g) { return g.path == game.path; })) {
                unique.push_back(std::move(game));
            }
        }
        return unique;
    });
}

/// Boots the game at `path` on the GUI thread unless one is already running.
std::string QtMcpHost::BootGame(const std::string& path) {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (window.emulation_running) {
            return "a game is already running; stop it first";
        }
        window.BootGame(QString::fromStdString(path), window.ApplicationAppletParameters());
        return window.emulation_running ? std::string{} : "boot failed (see the timeline/logs)";
    });
}

/// Switches docked or handheld mode without the warning box, like the status bar toggle.
std::string QtMcpHost::SetDocked(bool docked) {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (!QtCommon::system) {
            return "no system";
        }
        const bool was_docked = Settings::IsDockedMode();
        if (was_docked == docked) {
            return {};
        }
        // MainWindow::OnToggleDockedMode, minus its warning box: handheld can't be used docked,
        // so player 1 becomes a Pro Controller.
        auto& hid = QtCommon::system->HIDCore();
        auto* player_1 = hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
        auto* handheld = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        if (docked && handheld->IsConnected()) {
            handheld->Disconnect();
            player_1->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
            player_1->Connect();
            window.controller_dialog->refreshConfiguration();
        }
        Settings::values.use_docked_mode.SetValue(docked ? Settings::ConsoleMode::Docked
                                                         : Settings::ConsoleMode::Handheld);
        window.UpdateDockedButton();
        OnDockedModeChanged(was_docked, docked, *QtCommon::system);
        return {};
    });
}

/// Applies changed settings to the running system on the GUI thread.
std::string QtMcpHost::ApplySettings() {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (window.emulation_running && QtCommon::system) {
            QtCommon::system->ApplySettings();
        }
        return {};
    });
}

/// Runs fn on the TAS engine on the GUI thread, then refreshes the TAS status.
std::string QtMcpHost::WithTas(bool require_game,
                               std::function<void(InputCommon::TasInput::Tas&)> fn) {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (require_game && !window.emulation_running) {
            return "no game is running";
        }
        if (window.emulation_running) {
            // As the TAS hotkeys do: keep TAS input from triggering a system hotkey.
            QtCommon::system->HIDCore()
                .GetEmulatedController(Core::HID::NpadIdType::Player1)
                ->ResetSystemButtons();
        }
        fn(*window.input_subsystem->GetTas());
        window.OnTasStateChanged();
        return {};
    });
}

/// Pauses the running game on the GUI thread.
std::string QtMcpHost::Pause() {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (!window.emulation_running) {
            return "no game is running";
        }
        window.OnPauseGame();
        return {};
    });
}

/// Resumes the paused game on the GUI thread.
std::string QtMcpHost::Resume() {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (!window.emulation_running) {
            return "no game is running";
        }
        window.OnStartGame();
        return {};
    });
}

/// Shuts down the running game on the GUI thread.
std::string QtMcpHost::Stop() {
    return RunOnGuiThread(&window, [&]() -> std::string {
        if (!window.emulation_running) {
            return "no game is running";
        }
        window.ShutdownGame();
        return {};
    });
}

/// Queues stopping the game and closing the main window.
std::string QtMcpHost::Quit() {
    // Queued, not blocking: closing destroys the MCP server, which must first finish this reply.
    MainWindow* main_window = &window;
    QMetaObject::invokeMethod(
        main_window,
        [main_window] {
            if (main_window->emulation_running) {
                main_window->ShutdownGame(); // no confirmation prompt, unlike closing mid-game
            }
            main_window->close();
        },
        Qt::QueuedConnection);
    return {};
}

/// Captures a screenshot and encodes it as PNG.
std::optional<std::vector<u8>> QtMcpHost::CaptureScreenshotPng(u32 max_width,
                                                               std::chrono::milliseconds timeout,
                                                               std::string& error) {
    const auto image = CaptureImage(max_width, timeout, error);
    if (!image) {
        return std::nullopt;
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image->save(&buffer, "PNG")) {
        error = "PNG encoding failed";
        return std::nullopt;
    }
    return std::vector<u8>(png.begin(), png.end());
}

/// Captures a screenshot as raw BGRA pixels.
std::optional<Mcp::Frame> QtMcpHost::CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                                  std::string& error) {
    const auto image = CaptureImage(width, timeout, error);
    if (!image) {
        return std::nullopt;
    }
    // Format_RGB32 is 0xffRRGGBB per pixel: B, G, R, A bytes in memory.
    Mcp::Frame frame{.width = static_cast<u32>(image->width()),
                     .height = static_cast<u32>(image->height())};
    frame.bgra.resize(size_t{frame.width} * frame.height * 4);
    for (u32 y = 0; y < frame.height; ++y) {
        std::memcpy(frame.bgra.data() + size_t{y} * frame.width * 4,
                    image->constScanLine(static_cast<int>(y)), size_t{frame.width} * 4);
    }
    return frame;
}

/// Asks the renderer for a 16:9 screenshot max_width wide and waits up to `timeout` for it.
std::optional<QImage> QtMcpHost::CaptureImage(u32 max_width, std::chrono::milliseconds timeout,
                                              std::string& error) {
    if (!window.emulation_running || !QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        error = "no game is running";
        return std::nullopt;
    }
    auto& renderer = QtCommon::system->Renderer();
    if (renderer.IsScreenshotPending()) {
        error = "another screenshot is in progress; retry";
        return std::nullopt;
    }
    const u32 width = std::clamp<u32>(max_width, 160, 3840);
    const u32 height = width * 9 / 16;
    const Layout::FramebufferLayout layout = Layout::DefaultFrameLayout(width, height);

    // Shared with the renderer's callback, which may run after we time out.
    struct Capture {
        QImage image;
        std::promise<bool> done;
    };
    auto capture = std::make_shared<Capture>();
    capture->image = QImage(QSize(static_cast<int>(width), static_cast<int>(height)),
                            QImage::Format_RGB32);
    auto future = capture->done.get_future();
    renderer.RequestScreenshot(
        capture->image.bits(),
        [capture](bool invert_y) {
            if (invert_y) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                capture->image = capture->image.flipped(Qt::Vertical);
#else
                capture->image = capture->image.mirrored(false, true);
#endif
            }
            capture->done.set_value(true);
        },
        layout);

    if (future.wait_for(timeout) != std::future_status::ready) {
        error = "timed out waiting for a frame (the game may be paused, loading, or the window "
                "hidden)";
        return std::nullopt;
    }
    return capture->image;
}
