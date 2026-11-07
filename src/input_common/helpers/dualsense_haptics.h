// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2026 Three-Dola
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <vector>

#include <SDL3/SDL.h>

#include "common/common_types.h"

namespace InputCommon {

// Drives the DualSense voice coil actuators through the controller's USB audio device. The
// actuators are exposed as channels 3 and 4 of a 4 channel playback device, so HD rumble can be
// reproduced by synthesizing the requested frequencies instead of collapsing them into motor
// strengths.
class DualSenseHaptics {
public:
    DualSenseHaptics() = default;
    ~DualSenseHaptics();

    DualSenseHaptics(const DualSenseHaptics&) = delete;
    DualSenseHaptics& operator=(const DualSenseHaptics&) = delete;

    // Opens the first unused DualSense audio device. Returns false if none is available.
    bool Open();
    void Close();

    bool IsOpen() const {
        return stream != nullptr;
    }

    // Amplitudes are in the range [0, 1], frequencies in Hz.
    void SetVibration(f32 low_amplitude, f32 low_frequency, f32 high_amplitude,
                      f32 high_frequency);

private:
    struct Band {
        f32 amplitude{};
        f32 frequency{};
        f64 phase{};
    };

    static void SDLCALL AudioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additional_amount, int total_amount);
    static void Advance(Band& band, const Band& target);
    void Render(int frames);
    void UpdateTargets();

    SDL_AudioStream* stream{};

    std::atomic<f32> target_low_amplitude{};
    std::atomic<f32> target_low_frequency{160.0f};
    std::atomic<f32> target_high_amplitude{};
    std::atomic<f32> target_high_frequency{320.0f};
    std::atomic<u64> last_update_ms{};

    // Only touched from the audio callback
    Band low{.frequency = 160.0f};
    Band high{.frequency = 320.0f};
    Band low_target{.frequency = 160.0f};
    Band high_target{.frequency = 320.0f};
    int frames_until_update{};
    std::vector<f32> buffer;
};

} // namespace InputCommon
