// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "ebridge/host.h"

namespace InputCommon {
class InputSubsystem;
}

namespace EBridge {

// Eglantine Bridge: plays the running game on a Nintendo Switch 1 over the network.
// The console app (switch-client) sends its controls, which drive the emulated
// player 1 and handheld controllers through the virtual gamepad; the renderer,
// audio and vibration taps (common/ebridge_hooks.h) send video, sound and rumble back.
class BridgeService {
public:
    using EventCallback = std::function<void(ebridge::Event, const std::string&)>;

    explicit BridgeService(std::shared_ptr<InputCommon::InputSubsystem> input);
    ~BridgeService();

    using LaunchCallback = std::function<void(const ebridge::GameInfo&)>;

    // Both run on the bridge's network threads: hop to the UI thread before touching widgets.
    void SetEventCallback(EventCallback cb);
    void SetLaunchCallback(LaunchCallback cb);

    // The games the console can start (from the game list). Thread-safe.
    void SetGames(std::vector<ebridge::GameInfo> list);
    // Name shown for a game file, from that list (empty if unknown).
    std::string NameForPath(const std::string& path);

    // What the emulator is doing, for the console's loading screen (EB_STATUS_*).
    void SetStatus(int state, u32 value, u32 total, const std::string& text,
                   const std::string& game);

    bool Start(const ebridge::HostConfig& config, std::string* error);
    void Stop();

private:
    void OnInput(const ebridge::InputState& s);
    void OnRumble(std::size_t player_index, f32 amp_low, f32 freq_low, f32 amp_high,
                  f32 freq_high);
    void ReleaseAll();

    std::shared_ptr<InputCommon::InputSubsystem> input;
    std::unique_ptr<ebridge::Host> host;
    EventCallback on_event;
    LaunchCallback on_launch;
    std::mutex games_mutex;
    std::vector<ebridge::GameInfo> games;
    u32 prev_buttons = 0;
    int prev_touch_count = 0;
    std::array<f32, 4> last_rumble{};

    std::atomic<s64> last_video_ms{0};
    std::atomic<bool> idle_running{false};
    std::thread idle_thread;
};

} // namespace EBridge
