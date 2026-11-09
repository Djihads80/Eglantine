// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "common/game_presets.h"
#include "common/settings.h"
#include "common/thread.h"

namespace {
void SetConflictingPreferences(Settings::Values& settings, bool per_game) {
    const auto set = [per_game](auto& setting, auto value) {
        setting.SetGlobal(!per_game);
        setting.SetValue(value);
    };
    set(settings.renderer_backend, Settings::RendererBackend::Null);
    set(settings.resolution_setup, Settings::ResolutionSetup::Res2X);
    set(settings.gpu_accuracy, Settings::GpuAccuracy::High);
    set(settings.use_asynchronous_shaders, false);
    set(settings.astc_recompression, Settings::AstcRecompression::Uncompressed);
    set(settings.vram_usage_mode, Settings::VramUsageMode::Aggressive);
    set(settings.use_reactive_flushing, true);
    set(settings.vsync_mode, Settings::VSyncMode::Immediate);
    set(settings.scaling_filter, Settings::ScalingFilter::NearestNeighbor);
    set(settings.anti_aliasing, Settings::AntiAliasing::Smaa);
}

void CheckPreset(const Settings::Values& settings, u64 program_id) {
    CHECK(settings.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan);
    CHECK(settings.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1X);
    CHECK(settings.gpu_accuracy.GetValue() == Settings::GpuAccuracy::Low);
    CHECK(settings.use_asynchronous_shaders.GetValue());
    CHECK(settings.astc_recompression.GetValue() == Settings::AstcRecompression::Bc3);
    CHECK(settings.vram_usage_mode.GetValue() == Settings::VramUsageMode::Conservative);
    CHECK(settings.use_reactive_flushing.GetValue() ==
          (program_id == Settings::OcarinaOfTimeProgramId));
    CHECK(settings.vsync_mode.GetValue() == Settings::VSyncMode::FifoRelaxed);
    CHECK(settings.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked);
    CHECK(settings.scaling_filter.GetValue() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings.anti_aliasing.GetValue() == Settings::AntiAliasing::Fxaa);
}

void CheckPreferences(const Settings::Values& settings, bool global = false) {
    CHECK(settings.renderer_backend.GetValue(global) == Settings::RendererBackend::Null);
    CHECK(settings.resolution_setup.GetValue(global) == Settings::ResolutionSetup::Res2X);
    CHECK(settings.gpu_accuracy.GetValue(global) == Settings::GpuAccuracy::High);
    CHECK_FALSE(settings.use_asynchronous_shaders.GetValue(global));
    CHECK(settings.astc_recompression.GetValue(global) ==
          Settings::AstcRecompression::Uncompressed);
    CHECK(settings.vram_usage_mode.GetValue(global) == Settings::VramUsageMode::Aggressive);
    CHECK(settings.use_reactive_flushing.GetValue(global));
    CHECK(settings.vsync_mode.GetValue(global) == Settings::VSyncMode::Immediate);
    CHECK(settings.scaling_filter.GetValue(global) == Settings::ScalingFilter::NearestNeighbor);
    CHECK(settings.anti_aliasing.GetValue(global) == Settings::AntiAliasing::Smaa);
}
} // namespace

TEST_CASE("Display filter defaults use ScaleForce and FXAA", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    CHECK(settings->scaling_filter.GetDefault() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings->anti_aliasing.GetDefault() == Settings::AntiAliasing::Fxaa);
}

TEST_CASE("Catalogue recommendations refer to valid per-game settings", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    for (const auto& [program_id, preset] : Settings::game_presets) {
        INFO(program_id);
        REQUIRE(Settings::FindGamePreset(program_id) == &preset);
        REQUIRE_FALSE(preset.settings.empty());
        for (const auto& recommendation : preset.settings) {
            INFO(recommendation.key);
            const auto it = settings->linkage.by_key.find(recommendation.key);
            REQUIRE(it != settings->linkage.by_key.end());
            REQUIRE(it->second->Switchable());
        }
        Settings::SetRecommendedSettings(*settings, preset, false);
        for (const auto& recommendation : preset.settings) {
            const auto* setting = settings->linkage.by_key.at(recommendation.key);
            CHECK(setting->ToString() == recommendation.value);
            CHECK_FALSE(setting->UsingGlobal());
        }
    }
}

TEST_CASE("A preset can override a subset of settings", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    const Settings::GamePreset preset{"Example", {{"speed_limit", 75U}}};
    const auto saved_speed = settings->speed_limit.GetValue(true);
    Settings::SetRecommendedSettings(*settings, preset, false);
    CHECK(settings->speed_limit.GetValue() == 75U);
    CHECK(settings->speed_limit.GetValue(true) == saved_speed);
    CHECK_FALSE(settings->speed_limit.UsingGlobal());
    CheckPreferences(*settings);
    for (const auto& restore : settings->linkage.restore_functions) {
        restore();
    }
    CHECK(settings->speed_limit.GetValue() == saved_speed);
}

TEST_CASE("Accepted recommended settings replace visible global preferences", "[game_presets]") {
    const u64 program_id =
        GENERATE(Settings::MarioKartWorldProgramId, Settings::OcarinaOfTimeProgramId);
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    const auto* preset = Settings::FindGamePreset(program_id);
    REQUIRE(preset != nullptr);
    Settings::SetRecommendedSettings(*settings, *preset, true);
    CheckPreset(*settings, program_id);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->gpu_accuracy.UsingGlobal());
    CHECK(settings->renderer_backend.GetValue(true) == Settings::RendererBackend::Vulkan);
    CHECK(settings->gpu_accuracy.GetValue(true) == Settings::GpuAccuracy::Low);
}

TEST_CASE("Game preset overrides saved settings for the session", "[game_presets]") {
    const u64 program_id =
        GENERATE(Settings::MarioKartWorldProgramId, Settings::OcarinaOfTimeProgramId);
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    SetConflictingPreferences(*settings, true);
    REQUIRE(Settings::ApplyGamePreset(*settings, program_id, true));
    CheckPreset(*settings, program_id);
    CheckPreferences(*settings, true);
    for (const auto& restore : settings->linkage.restore_functions) {
        restore();
    }
    CheckPreferences(*settings);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->resolution_setup.UsingGlobal());
    REQUIRE(Settings::ApplyGamePreset(*settings, program_id, true));
    CheckPreset(*settings, program_id);
}

TEST_CASE("Game preset does not change unrelated games", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    CHECK_FALSE(Settings::ApplyGamePreset(*settings, 0, true));
    CHECK_FALSE(
        Settings::ApplyGamePreset(*settings, Settings::MarioKartWorldProgramId + 1, true));
    CheckPreferences(*settings);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->resolution_setup.UsingGlobal());
}

TEST_CASE("Declining a game preset preserves global and per-game preferences", "[game_presets]") {
    const u64 program_id =
        GENERATE(Settings::MarioKartWorldProgramId, Settings::OcarinaOfTimeProgramId);
    auto settings = std::make_unique<Settings::Values>();
    for (bool per_game : {false, true}) {
        SetConflictingPreferences(*settings, per_game);
        CHECK_FALSE(Settings::ApplyGamePreset(*settings, program_id, false));
        CheckPreferences(*settings);
        CHECK(settings->renderer_backend.UsingGlobal() == !per_game);
        CHECK(settings->resolution_setup.UsingGlobal() == !per_game);
    }
}

TEST_CASE("3D V-Cache thread placement and live RefreshThreadPolicies", "[common][thread_affinity]") {
    CHECK(Settings::values.prefer_3d_vcache.GetDefault());
#ifdef _WIN32
    const bool prev_setting = Settings::values.prefer_3d_vcache.GetValue();
    Settings::values.prefer_3d_vcache.SetValue(true);

    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask));

    const auto query_current_thread_mask = [process_mask]() -> DWORD_PTR {
        const DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), process_mask);
        if (prev != 0) {
            SetThreadAffinityMask(GetCurrentThread(), prev);
        }
        return prev;
    };

    DWORD_PTR perf_mask = 0;
    DWORD_PTR eff_mask = 0;
    DWORD_PTR unrestricted_after_disable = 0;
    DWORD_PTR restored_after_enable = 0;

    std::thread worker([&] {
        Common::SetCurrentThreadToPerformanceCores();
        perf_mask = query_current_thread_mask();

        Common::SetCurrentThreadToEfficiencyCores();
        eff_mask = query_current_thread_mask();

        Settings::values.prefer_3d_vcache.SetValue(false);
        Common::RefreshThreadPolicies();
        unrestricted_after_disable = query_current_thread_mask();

        Settings::values.prefer_3d_vcache.SetValue(true);
        Common::RefreshThreadPolicies();
        restored_after_enable = query_current_thread_mask();
    });
    worker.join();

    Settings::values.prefer_3d_vcache.SetValue(prev_setting);
    Common::RefreshThreadPolicies();

    CHECK(perf_mask != 0);
    CHECK(eff_mask != 0);
    if (perf_mask != process_mask || eff_mask != process_mask) {
        CHECK((perf_mask & eff_mask) == 0);
        CHECK((perf_mask | eff_mask) == process_mask);
        CHECK(unrestricted_after_disable == process_mask);
        CHECK(restored_after_enable == eff_mask);
    }
#endif
}
