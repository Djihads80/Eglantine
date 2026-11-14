// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/settings.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 HyruleWarriorsProgramId = 0x04003F40232DE000ULL;

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
       {"use_docked_mode", ConsoleMode::Docked},
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

// Settings that only affect one game. Per-game configuration shows them in that game's Tweaks tab.
inline const std::map<u64, std::vector<std::string>> game_tweaks{
    {MarioKartWorldProgramId, {"mkw_30fps", "mkw_dynamic_fps"}},
    {HyruleWarriorsProgramId, {"hyrule_dynamic_fps", "hyrule_30fps"}},
};

inline const std::vector<std::string>* FindGameTweaks(u64 program_id) {
    const auto it = game_tweaks.find(program_id);
    return it == game_tweaks.end() ? nullptr : &it->second;
}

inline bool IsGameTweak(const std::string& key) {
    for (const auto& [program_id, keys] : game_tweaks) {
        for (const auto& tweak : keys) {
            if (tweak == key) {
                return true;
            }
        }
    }
    return false;
}

inline const GamePreset* FindGamePreset(u64 program_id) {
    const auto it = game_presets.find(program_id);
    return it == game_presets.end() ? nullptr : &it->second;
}

// When `keys` is given, only the recommendations it names are applied.
inline void SetRecommendedSettings(Values& settings, const GamePreset& preset, bool global,
                                   const std::set<std::string>* keys = nullptr) {
    const auto selected = [keys](const std::string& key) {
        return keys == nullptr || keys->contains(key);
    };
    for (const auto& recommendation : preset.settings) {
        if (!selected(recommendation.key)) {
            continue;
        }
        auto* setting = settings.linkage.by_key.at(recommendation.key);
        setting->SetGlobal(global);
        setting->LoadString(recommendation.value);
    }
    // DLSS does its own upscaling and anti-aliasing.
    if (settings.nvidia_dlss.GetValue()) {
        if (selected("scaling_filter")) {
            settings.scaling_filter.SetGlobal(global);
            settings.scaling_filter.SetValue(ScalingFilter::Bilinear);
        }
        if (selected("anti_aliasing")) {
            settings.anti_aliasing.SetGlobal(global);
            settings.anti_aliasing.SetValue(AntiAliasing::None);
        }
    }
}

inline bool ApplyGamePreset(Values& settings, u64 program_id, bool accepted,
                            const std::set<std::string>* keys = nullptr) {
    const auto* preset = FindGamePreset(program_id);
    if (!accepted || preset == nullptr || (keys != nullptr && keys->empty())) {
        return false;
    }
    SetRecommendedSettings(settings, *preset, false, keys);
    return true;
}

} // namespace Settings
