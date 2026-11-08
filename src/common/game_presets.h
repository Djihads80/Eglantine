// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/settings.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;

struct RecommendedSetting {
    std::string key;
    std::string value;

    template <typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    RecommendedSetting(std::string key_, T value_) : key{std::move(key_)} {
        if constexpr (std::is_same_v<T, bool>) {
            value = value_ ? "true" : "false";
        } else if constexpr (std::is_enum_v<T>) {
            value = std::to_string(static_cast<std::underlying_type_t<T>>(value_));
        } else {
            value = std::to_string(value_);
        }
    }
};

struct GamePreset {
    const char* name;
    std::vector<RecommendedSetting> settings;
};

// Keys are registered setting labels; values use the same encoding as configuration files.
inline const std::map<u64, GamePreset> game_presets{
    {MarioKartWorldProgramId,
     {"Mario Kart World",
      {{"backend", RendererBackend::Vulkan},
       {"resolution_setup", ResolutionSetup::Res1X},
       {"gpu_accuracy", GpuAccuracy::Low},
       {"use_asynchronous_shaders", true},
       {"astc_recompression", AstcRecompression::Bc3},
       {"vram_usage_mode", VramUsageMode::Conservative},
       {"use_reactive_flushing", false},
       {"use_vsync", VSyncMode::FifoRelaxed},
       {"use_docked_mode", ConsoleMode::Handheld},
       {"scaling_filter", ScalingFilter::ScaleForce},
       {"anti_aliasing", AntiAliasing::Fxaa}}}},
    {OcarinaOfTimeProgramId,
     {"Ocarina of Time",
      {{"backend", RendererBackend::Vulkan},
       {"resolution_setup", ResolutionSetup::Res1X},
       {"gpu_accuracy", GpuAccuracy::Low},
       {"use_asynchronous_shaders", true},
       {"astc_recompression", AstcRecompression::Bc3},
       {"vram_usage_mode", VramUsageMode::Conservative},
       {"use_reactive_flushing", true},
       {"use_vsync", VSyncMode::FifoRelaxed},
       {"use_docked_mode", ConsoleMode::Docked},
       {"scaling_filter", ScalingFilter::ScaleForce},
       {"anti_aliasing", AntiAliasing::Fxaa}}}},
};

inline const GamePreset* FindGamePreset(u64 program_id) {
    const auto it = game_presets.find(program_id);
    return it == game_presets.end() ? nullptr : &it->second;
}

inline void SetRecommendedSettings(Values& settings, const GamePreset& preset, bool global) {
    for (const auto& recommendation : preset.settings) {
        auto* setting = settings.linkage.by_key.at(recommendation.key);
        setting->SetGlobal(global);
        setting->LoadString(recommendation.value);
    }
}

inline bool ApplyGamePreset(Values& settings, u64 program_id, bool accepted) {
    const auto* preset = FindGamePreset(program_id);
    if (!accepted || preset == nullptr) {
        return false;
    }
    SetRecommendedSettings(settings, *preset, false);
    return true;
}

} // namespace Settings
