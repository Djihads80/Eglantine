// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>

#include "common/common_types.h"

// Taps for Eglantine Bridge (streaming to a Nintendo Switch 1 over the network).
// The frontend installs the sinks; the renderer, audio and HID code feed them.
// Each Wants*() is a cheap atomic check so the hot paths cost nothing while off.
namespace Common::EBridge {

using VideoSink = std::function<void(const u8* bgra, u32 width, u32 height, u32 stride)>;
using AudioSink = std::function<void(const s16* stereo, std::size_t frames)>;
// `device` is the side of the controller that vibrates: 0 left, 1 right
// (Core::HID::DeviceIndex). Games set each side separately.
using RumbleSink = std::function<void(std::size_t player_index, std::size_t device, f32 amp_low,
                                      f32 freq_low, f32 amp_high, f32 freq_high)>;

void SetVideoSink(VideoSink sink);
void SetAudioSink(AudioSink sink);
void SetRumbleSink(RumbleSink sink);

// Enabled only while a console is connected.
void SetStreaming(bool streaming);

bool WantsVideo();
bool WantsAudio();

void SubmitVideo(const u8* bgra, u32 width, u32 height, u32 stride);
// 48 kHz interleaved samples with `channels` channels; converted to stereo here.
// `source` identifies the output stream: only one stream is forwarded at a time.
void SubmitAudio(const void* source, const s16* samples, std::size_t frames, std::size_t channels);
void SubmitRumble(std::size_t player_index, std::size_t device, f32 amp_low, f32 freq_low,
                  f32 amp_high, f32 freq_high);

} // namespace Common::EBridge
