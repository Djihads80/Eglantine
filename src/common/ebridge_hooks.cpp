// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "common/ebridge_hooks.h"

namespace Common::EBridge {
namespace {

std::atomic<bool> g_streaming{false};

// One mutex per sink, so video, audio and rumble never wait on each other.
// Setting a sink takes its mutex, so it also waits for a callback in progress:
// once a setter returns, the old sink is no longer running.
std::mutex g_video_mutex;
VideoSink g_video;

std::mutex g_rumble_mutex;
RumbleSink g_rumble;

// Audio, guarded by g_audio_mutex: the sink, the stream currently forwarded,
// and when it last delivered samples.
std::mutex g_audio_mutex;
AudioSink g_audio;
const void* g_audio_source = nullptr;
std::chrono::steady_clock::time_point g_audio_last{};
std::vector<s16> g_audio_scratch;

} // namespace

void SetVideoSink(VideoSink sink) {
    std::scoped_lock lk{g_video_mutex};
    g_video = std::move(sink);
}

void SetAudioSink(AudioSink sink) {
    std::scoped_lock lk{g_audio_mutex};
    g_audio = std::move(sink);
}

void SetRumbleSink(RumbleSink sink) {
    std::scoped_lock lk{g_rumble_mutex};
    g_rumble = std::move(sink);
}

void SetStreaming(bool streaming) {
    g_streaming.store(streaming, std::memory_order_relaxed);
}

bool WantsVideo() {
    return g_streaming.load(std::memory_order_relaxed);
}

bool WantsAudio() {
    return g_streaming.load(std::memory_order_relaxed);
}

void SubmitVideo(const u8* bgra, u32 width, u32 height, u32 stride) {
    std::scoped_lock lk{g_video_mutex};
    if (g_video) {
        g_video(bgra, width, height, stride);
    }
}

void SubmitAudio(const void* source, const s16* samples, std::size_t frames, std::size_t channels) {
    if (channels == 0 || frames == 0) {
        return;
    }
    std::scoped_lock lk{g_audio_mutex};
    if (!g_audio) {
        return;
    }
    // Several output streams would interleave into noise: keep the first one,
    // and switch only after it has been silent for a while.
    const auto now = std::chrono::steady_clock::now();
    if (source != g_audio_source) {
        if (g_audio_source && now - g_audio_last < std::chrono::seconds(1)) {
            return;
        }
        g_audio_source = source;
    }
    g_audio_last = now;

    if (channels == 2) {
        g_audio(samples, frames);
        return;
    }
    g_audio_scratch.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const s16* frame = samples + i * channels;
        g_audio_scratch[i * 2] = frame[0];
        g_audio_scratch[i * 2 + 1] = channels > 1 ? frame[1] : frame[0];
    }
    g_audio(g_audio_scratch.data(), frames);
}

void SubmitRumble(std::size_t player_index, std::size_t device, f32 amp_low, f32 freq_low,
                  f32 amp_high, f32 freq_high) {
    if (!g_streaming.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lk{g_rumble_mutex};
    if (g_rumble) {
        g_rumble(player_index, device, amp_low, freq_low, amp_high, freq_high);
    }
}

} // namespace Common::EBridge
