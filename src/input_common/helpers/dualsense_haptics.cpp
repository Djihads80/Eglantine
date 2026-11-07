// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2026 Three-Dola
// SPDX-License-Identifier: MIT

// Synthesis adapted from RenHDrum src/dualsense/haptics.rs: https://github.com/trzz1214/RenHDrum

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

#include "common/logging.h"
#include "input_common/helpers/dualsense_haptics.h"

namespace InputCommon {

namespace {
constexpr int SampleRate = 48000;
constexpr int Channels = 4;
constexpr int LeftActuatorChannel = 2;
constexpr int RightActuatorChannel = 3;

// Targets are picked up in fixed blocks so the feel doesn't depend on the host buffer size
constexpr int UpdateFrames = 256;

// Keeps sustained output away from the level where the coils heat up and distort
constexpr f32 OutputGain = 0.8f;

// One pole smoothing with a 4ms time constant on amplitude and frequency changes
const f32 SmoothingFactor = static_cast<f32>(1.0 - std::exp(-1000.0 / (SampleRate * 4.0)));

constexpr f32 MinFrequency = 20.0f;
constexpr f32 MaxFrequency = 1000.0f;
constexpr f32 DefaultLowFrequency = 160.0f;
constexpr f32 DefaultHighFrequency = 320.0f;

// Matches the duration SDL rumble requests are sent with
constexpr u64 VibrationTimeoutMs = 2000;

// Only one controller can own the audio device
std::atomic<bool> device_in_use{false};

f32 SanitizeAmplitude(f32 amplitude) {
    if (!std::isfinite(amplitude)) {
        return 0.0f;
    }
    // Square root curve so faint vibrations stay above what can be felt on the actuators
    return std::sqrt(std::clamp(amplitude, 0.0f, 1.0f));
}

int CountWiredDualSenses() {
    int gamepad_count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepad_count);
    int count = 0;
    for (int i = 0; gamepads != nullptr && i < gamepad_count; ++i) {
        if (SDL_GetGamepadTypeForID(gamepads[i]) != SDL_GAMEPAD_TYPE_PS5) {
            continue;
        }
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(gamepads[i]);
        if (gamepad != nullptr &&
            SDL_GetGamepadConnectionState(gamepad) == SDL_JOYSTICK_CONNECTION_WIRED) {
            ++count;
        }
    }
    SDL_free(gamepads);
    return count;
}

f32 SanitizeFrequency(f32 frequency, f32 fallback) {
    if (!(frequency > 0.0f)) {
        return fallback;
    }
    return std::clamp(frequency, MinFrequency, MaxFrequency);
}
} // Anonymous namespace

DualSenseHaptics::~DualSenseHaptics() {
    Close();
}

bool DualSenseHaptics::Open() {
    if (stream != nullptr) {
        return true;
    }
    if (device_in_use.exchange(true)) {
        return false;
    }

    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR(Input, "Failed to initialize SDL audio for DualSense haptics: {}",
                  SDL_GetError());
        device_in_use = false;
        return false;
    }

    int device_count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&device_count);
    SDL_AudioDeviceID haptics_device = 0;
    std::string haptics_device_name;
    int candidate_count = 0;
    for (int i = 0; devices != nullptr && i < device_count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (name == nullptr || std::strstr(name, "DualSense") == nullptr) {
            continue;
        }
        SDL_AudioSpec device_spec{};
        if (!SDL_GetAudioDeviceFormat(devices[i], &device_spec, nullptr) ||
            device_spec.channels < Channels) {
            continue;
        }
        haptics_device = devices[i];
        haptics_device_name = name;
        ++candidate_count;
    }
    SDL_free(devices);

    // SDL doesn't tell which audio device belongs to which gamepad. With more than one wired
    // DualSense the haptics could end up on the wrong controller, so use standard rumble instead.
    // Gamepads are counted too, since a second controller's audio device can show up late.
    const int gamepad_count = CountWiredDualSenses();
    if (candidate_count != 1 || gamepad_count != 1) {
        if (candidate_count > 1 || gamepad_count > 1) {
            LOG_DEBUG(Input,
                      "Found {} DualSense audio devices and {} wired DualSense controllers, "
                      "using standard rumble",
                      candidate_count, gamepad_count);
        }
        device_in_use = false;
        return false;
    }
    LOG_INFO(Input, "Using audio device \"{}\" for DualSense haptics", haptics_device_name);

    const SDL_AudioSpec spec{
        .format = SDL_AUDIO_F32,
        .channels = Channels,
        .freq = SampleRate,
    };
    stream = SDL_OpenAudioDeviceStream(haptics_device, &spec, AudioCallback, this);
    if (stream == nullptr) {
        LOG_ERROR(Input, "Failed to open DualSense haptics stream: {}", SDL_GetError());
        device_in_use = false;
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        LOG_ERROR(Input, "Failed to start DualSense haptics stream: {}", SDL_GetError());
        Close();
        return false;
    }
    return true;
}

void DualSenseHaptics::Close() {
    if (stream == nullptr) {
        return;
    }
    SDL_DestroyAudioStream(stream);
    stream = nullptr;
    device_in_use = false;
}

void DualSenseHaptics::SetVibration(f32 low_amplitude_, f32 low_frequency_, f32 high_amplitude_,
                                    f32 high_frequency_) {
    f32 low_amp = SanitizeAmplitude(low_amplitude_);
    f32 high_amp = SanitizeAmplitude(high_amplitude_);

    // Both bands are mixed into the same signal, keep the sum within full scale
    const f32 total = low_amp + high_amp;
    if (total > 1.0f) {
        low_amp /= total;
        high_amp /= total;
    }

    target_low_amplitude = low_amp;
    target_low_frequency = SanitizeFrequency(low_frequency_, DefaultLowFrequency);
    target_high_amplitude = high_amp;
    target_high_frequency = SanitizeFrequency(high_frequency_, DefaultHighFrequency);
    last_update_ms = SDL_GetTicks();
}

void SDLCALL DualSenseHaptics::AudioCallback(void* userdata, SDL_AudioStream* stream,
                                             int additional_amount, int total_amount) {
    if (additional_amount <= 0) {
        return;
    }
    auto* haptics = static_cast<DualSenseHaptics*>(userdata);
    const int frames = additional_amount / static_cast<int>(sizeof(f32) * Channels);
    haptics->Render(frames);
    SDL_PutAudioStreamData(stream, haptics->buffer.data(),
                           static_cast<int>(haptics->buffer.size() * sizeof(f32)));
}

void DualSenseHaptics::Advance(Band& band, const Band& target) {
    band.amplitude += (target.amplitude - band.amplitude) * SmoothingFactor;
    band.frequency += (target.frequency - band.frequency) * SmoothingFactor;
    band.phase += static_cast<f64>(band.frequency) / SampleRate;
    if (band.phase >= 1.0) {
        band.phase -= 1.0;
    }
}

void DualSenseHaptics::UpdateTargets() {
    const bool timed_out = SDL_GetTicks() - last_update_ms.load() > VibrationTimeoutMs;
    low_target.amplitude = timed_out ? 0.0f : target_low_amplitude.load();
    low_target.frequency = target_low_frequency.load();
    high_target.amplitude = timed_out ? 0.0f : target_high_amplitude.load();
    high_target.frequency = target_high_frequency.load();
    frames_until_update = UpdateFrames;
}

void DualSenseHaptics::Render(int frames) {
    constexpr f64 TwoPi = 2.0 * std::numbers::pi;
    buffer.assign(static_cast<std::size_t>(frames) * Channels, 0.0f);

    for (int i = 0; i < frames; ++i) {
        if (frames_until_update == 0) {
            UpdateTargets();
        }
        --frames_until_update;

        Advance(low, low_target);
        Advance(high, high_target);

        const f64 value = low.amplitude * std::sin(TwoPi * low.phase) +
                          high.amplitude * std::sin(TwoPi * high.phase);
        const f32 sample = static_cast<f32>(std::clamp(value, -1.0, 1.0)) * OutputGain;

        f32* frame = &buffer[static_cast<std::size_t>(i) * Channels];
        frame[LeftActuatorChannel] = sample;
        frame[RightActuatorChannel] = sample;
    }
}

} // namespace InputCommon
