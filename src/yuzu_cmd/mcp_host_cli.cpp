// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <future>
#include <memory>

#include "common/stb.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"
#include "yuzu_cmd/mcp_host_cli.h"

CliMcpHost::CliMcpHost(Core::System& system_, InputCommon::InputSubsystem& input_)
    : system{system_}, input{input_} {}

std::string CliMcpHost::FrontendName() const {
    return "eden-cli";
}

Mcp::PerfSnapshot CliMcpHost::GetPerfSnapshot() {
    std::scoped_lock lock{perf_mutex};
    return perf;
}

InputCommon::InputSubsystem* CliMcpHost::GetInputSubsystem() {
    return &input;
}

std::string CliMcpHost::BootGame(const std::string&) {
    return "eden-cli runs the game given on its command line; relaunch it with another game";
}

/// Queues fn for the main thread and waits up to 30 s for its result.
std::string CliMcpHost::RunOnMain(std::function<std::string()> fn) {
    auto task = std::make_shared<std::packaged_task<std::string()>>(std::move(fn));
    auto result = task->get_future();
    {
        std::scoped_lock lock{queue_mutex};
        queue.emplace_back([task] { (*task)(); });
    }
    if (result.wait_for(std::chrono::seconds{30}) != std::future_status::ready) {
        return "timed out waiting for the main thread";
    }
    return result.get();
}

/// Runs the queued main thread tasks and samples the perf counters every 500 ms.
void CliMcpHost::DrainMainThread() {
    std::vector<std::function<void()>> tasks;
    {
        std::scoped_lock lock{queue_mutex};
        tasks.swap(queue);
    }
    for (auto& task : tasks) {
        task();
    }
    // Nothing else resets the perf counters in eden-cli: sample them like Qt's status bar.
    const auto now = std::chrono::steady_clock::now();
    if (system.IsPoweredOn() && now - last_perf >= std::chrono::milliseconds{500}) {
        last_perf = now;
        const auto stats = system.GetAndResetPerfStats();
        std::scoped_lock lock{perf_mutex};
        perf = {.valid = true,
                .game_fps = stats.average_game_fps,
                .emulation_speed = stats.emulation_speed,
                .frametime_ms = stats.frametime * 1000.0,
                .shaders_building = static_cast<u32>(system.GPU().ShaderNotify().NumBuildingNoReport())};
    }
}

std::string CliMcpHost::Pause() {
    return RunOnMain([this] {
        void(system.Pause());
        return std::string{};
    });
}

std::string CliMcpHost::Resume() {
    return RunOnMain([this] {
        void(system.Run());
        return std::string{};
    });
}

std::string CliMcpHost::Stop() {
    // eden-cli has no "no game" state: stopping the game ends the app.
    quit_requested = true;
    return {};
}

std::string CliMcpHost::Quit() {
    quit_requested = true;
    return {};
}

/// Applies changed settings to the running system on the main thread.
std::string CliMcpHost::ApplySettings() {
    return RunOnMain([this] {
        if (system.IsPoweredOn()) {
            system.ApplySettings();
        }
        return std::string{};
    });
}

/// Runs fn on the TAS engine on the main thread.
std::string CliMcpHost::WithTas(bool require_game,
                                std::function<void(InputCommon::TasInput::Tas&)> fn) {
    return RunOnMain([this, require_game, fn = std::move(fn)]() -> std::string {
        if (require_game && !system.IsPoweredOn()) {
            return "no game is running";
        }
        if (system.IsPoweredOn()) {
            system.HIDCore()
                .GetEmulatedController(Core::HID::NpadIdType::Player1)
                ->ResetSystemButtons();
        }
        fn(*input.GetTas());
        return {};
    });
}

/// Asks the renderer for a 16:9 BGRA frame `width` wide and waits up to `timeout` for it.
std::optional<Mcp::Frame> CliMcpHost::CaptureFrame(u32 width, std::chrono::milliseconds timeout,
                                                   std::string& error) {
    if (!system.IsPoweredOn()) {
        error = "no game is running";
        return std::nullopt;
    }
    auto& renderer = system.Renderer();
    if (renderer.IsScreenshotPending()) {
        error = "another screenshot is in progress; retry";
        return std::nullopt;
    }
    width = std::clamp<u32>(width, 160, 3840);
    const u32 height = width * 9 / 16;
    // Shared with the renderer's callback, which may run after we time out.
    struct Capture {
        Mcp::Frame frame;
        std::promise<bool> done;
    };
    auto capture = std::make_shared<Capture>();
    capture->frame = {.width = width, .height = height};
    capture->frame.bgra.resize(size_t{width} * height * 4);
    auto future = capture->done.get_future();
    renderer.RequestScreenshot(
        capture->frame.bgra.data(),
        [capture](bool invert_y) {
            if (invert_y) {
                auto& f = capture->frame;
                std::vector<u8> row(size_t{f.width} * 4);
                for (u32 y = 0; y < f.height / 2; ++y) {
                    u8* a = f.bgra.data() + size_t{y} * f.width * 4;
                    u8* b = f.bgra.data() + size_t{f.height - 1 - y} * f.width * 4;
                    std::memcpy(row.data(), a, row.size());
                    std::memcpy(a, b, row.size());
                    std::memcpy(b, row.data(), row.size());
                }
            }
            capture->done.set_value(true);
        },
        Layout::DefaultFrameLayout(width, height));
    if (future.wait_for(timeout) != std::future_status::ready) {
        error = "timed out waiting for a frame (paused, loading, or a hidden/null window: "
                "screenshot_raw works without one)";
        return std::nullopt;
    }
    return capture->frame;
}

/// Captures a frame and encodes it as an opaque RGBA PNG.
std::optional<std::vector<u8>> CliMcpHost::CaptureScreenshotPng(u32 max_width,
                                                                std::chrono::milliseconds timeout,
                                                                std::string& error) {
    auto frame = CaptureFrame(max_width, timeout, error);
    if (!frame) {
        return std::nullopt;
    }
    for (size_t i = 0; i + 3 < frame->bgra.size(); i += 4) {
        std::swap(frame->bgra[i], frame->bgra[i + 2]); // BGRA -> RGBA
        frame->bgra[i + 3] = 255;
    }
    std::vector<u8> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
        },
        &png, static_cast<int>(frame->width), static_cast<int>(frame->height), 4,
        frame->bgra.data(), static_cast<int>(frame->width * 4));
    return png;
}
