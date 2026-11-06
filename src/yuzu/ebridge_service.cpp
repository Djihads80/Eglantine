// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/ebridge_service.h"

#include <chrono>
#include <vector>

#include "common/ebridge_hooks.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"

namespace EBridge {
namespace {

// The console drives both player 1 and the handheld controller, so it works in
// either mode without any controller configuration.
constexpr std::array<std::size_t, 2> TargetPorts{0, 8};
constexpr std::size_t HandheldIndex = 8;

// Keeps the console's fingers apart from the ones made with the mouse.
constexpr std::size_t TouchIdBase = 100;

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

BridgeService::BridgeService(std::shared_ptr<InputCommon::InputSubsystem> input_)
    : input{std::move(input_)} {}

BridgeService::~BridgeService() {
    Stop();
}

void BridgeService::SetEventCallback(EventCallback cb) {
    on_event = std::move(cb);
}

void BridgeService::SetLaunchCallback(LaunchCallback cb) {
    on_launch = std::move(cb);
}

void BridgeService::SetGames(std::vector<ebridge::GameInfo> list) {
    std::scoped_lock lk{games_mutex};
    games = std::move(list);
}

std::string BridgeService::NameForPath(const std::string& path) {
    std::scoped_lock lk{games_mutex};
    for (const auto& game : games) {
        if (game.id == path) {
            return game.name;
        }
    }
    return {};
}

void BridgeService::SetStatus(int state, u32 value, u32 total, const std::string& text,
                              const std::string& game) {
    if (host) {
        host->SetStatus(state, value, total, text, game);
    }
}

bool BridgeService::Start(const ebridge::HostConfig& config, std::string* error) {
    Stop();
    host = std::make_unique<ebridge::Host>(config);
    host->SetInputCallback([this](const ebridge::InputState& s) { OnInput(s); });
    host->SetGameCallbacks(
        [this] {
            std::scoped_lock lk{games_mutex};
            return games;
        },
        [this](const ebridge::GameInfo& game) {
            if (on_launch) {
                on_launch(game);
            }
        });
    host->SetEventCallback([this](ebridge::Event e, const std::string& detail) {
        if (e == ebridge::Event::Connected) {
            {
                std::scoped_lock lk{rumble_mutex};
                rumble_sides = {};
                last_rumble = {};
            }
            Common::EBridge::SetStreaming(true);
        } else if (e == ebridge::Event::Disconnected) {
            Common::EBridge::SetStreaming(false);
            ReleaseAll();
        }
        if (on_event) {
            on_event(e, detail);
        }
    });
    if (!host->Start(error)) {
        host.reset();
        return false;
    }

    ebridge::Host* h = host.get();
    Common::EBridge::SetVideoSink([this, h](const u8* bgra, u32 width, u32 height, u32 stride) {
        last_video_ms.store(NowMs(), std::memory_order_relaxed);
        h->SubmitVideoFrame(bgra, width, height, stride, 0);
    });
    Common::EBridge::SetAudioSink(
        [h](const s16* stereo, std::size_t frames) { h->SubmitAudio(stereo, frames); });
    Common::EBridge::SetRumbleSink(
        [this](std::size_t player, std::size_t device, f32 al, f32 fl, f32 ah, f32 fh) {
            OnRumble(player, device, al, fl, ah, fh);
        });

    // With no game running the renderer sends nothing: show a grey picture instead,
    // so the console can tell "connected, no game" from a network problem.
    idle_running = true;
    idle_thread = std::thread([this, h] {
        std::vector<u8> grey(static_cast<std::size_t>(1280) * 720 * 4, 40);
        while (idle_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (h->IsStreaming() &&
                NowMs() - last_video_ms.load(std::memory_order_relaxed) > 500) {
                h->SubmitVideoFrame(grey.data(), 1280, 720, 1280 * 4, 0);
            }
        }
    });
    return true;
}

void BridgeService::Stop() {
    if (!host) {
        return;
    }
    idle_running = false;
    if (idle_thread.joinable()) {
        idle_thread.join();
    }
    Common::EBridge::SetStreaming(false);
    Common::EBridge::SetVideoSink({});
    Common::EBridge::SetAudioSink({});
    Common::EBridge::SetRumbleSink({});
    host->Stop();
    host.reset();
    ReleaseAll();
}

void BridgeService::OnInput(const ebridge::InputState& s) {
    auto* pad = input->GetVirtualGamepad();
    if (!pad) {
        return;
    }
    const u32 changed = s.buttons ^ prev_buttons;
    for (const std::size_t port : TargetPorts) {
        for (int i = 0; i < ebridge::kButtonCount; ++i) {
            if (changed & (1u << i)) {
                pad->SetButtonState(port, i, ((s.buttons >> i) & 1) != 0);
            }
        }
        pad->SetStickPosition(port, InputCommon::VirtualGamepad::VirtualStick::Left, s.lx, s.ly);
        pad->SetStickPosition(port, InputCommon::VirtualGamepad::VirtualStick::Right, s.rx, s.ry);
        pad->SetMotionState(port, s.delta_us, s.gyro[0], s.gyro[1], s.gyro[2], s.accel[0],
                            s.accel[1], s.accel[2]);
    }
    prev_buttons = s.buttons;

    auto* touch = input->GetTouchScreen();
    if (touch && (s.touch_count > 0 || prev_touch_count > 0)) {
        touch->ClearActiveFlag();
        for (int i = 0; i < s.touch_count; ++i) {
            touch->TouchMoved(s.touch[i].x, s.touch[i].y,
                              TouchIdBase + static_cast<std::size_t>(s.touch[i].id));
        }
        touch->ReleaseInactiveTouch();
    }
    prev_touch_count = s.touch_count;
}

void BridgeService::OnRumble(std::size_t player_index, std::size_t device, f32 amp_low,
                             f32 freq_low, f32 amp_high, f32 freq_high) {
    if ((player_index != 0 && player_index != HandheldIndex) || device >= rumble_sides.size()) {
        return;
    }
    std::array<f32, 4> value;
    {
        std::scoped_lock lk{rumble_mutex};
        // The console has one rumble channel: merge both sides so neither overwrites the
        // other. Each band takes the stronger side, with that side's frequency.
        rumble_sides[device] = {amp_low, freq_low, amp_high, freq_high};
        const auto& l = rumble_sides[0];
        const auto& r = rumble_sides[1];
        const auto& low = r[0] > l[0] ? r : l;
        const auto& high = r[2] > l[2] ? r : l;
        value = {low[0], low[1], high[2], high[3]};
        // Games repeat the same value every frame; only changes go over the network.
        if (value == last_rumble) {
            return;
        }
        last_rumble = value;
    }
    host->SendRumble(0, value[0], value[1], value[2], value[3]);
}

void BridgeService::ReleaseAll() {
    prev_buttons = 0;
    prev_touch_count = 0;
    if (auto* pad = input->GetVirtualGamepad()) {
        pad->ResetControllers();
    }
    if (auto* touch = input->GetTouchScreen()) {
        for (std::size_t id = 0; id < ebridge::kMaxTouch; ++id) {
            touch->TouchReleased(TouchIdBase + id);
        }
    }
}

} // namespace EBridge
