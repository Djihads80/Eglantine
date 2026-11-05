// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "common/game_presets.h"

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
    set(settings.mkw_dynamic_fps, false);
}

void CheckPreset(const Settings::Values& settings) {
    CHECK(settings.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan);
    CHECK(settings.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1X);
    CHECK(settings.gpu_accuracy.GetValue() == Settings::GpuAccuracy::Low);
    CHECK(settings.use_asynchronous_shaders.GetValue());
    CHECK(settings.astc_recompression.GetValue() == Settings::AstcRecompression::Bc3);
    CHECK(settings.vram_usage_mode.GetValue() == Settings::VramUsageMode::Conservative);
    CHECK_FALSE(settings.use_reactive_flushing.GetValue());
    CHECK(settings.vsync_mode.GetValue() == Settings::VSyncMode::FifoRelaxed);
    CHECK(settings.use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld);
    CHECK(settings.scaling_filter.GetValue() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings.anti_aliasing.GetValue() == Settings::AntiAliasing::Fxaa);
    CHECK(settings.mkw_dynamic_fps.GetValue());
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

TEST_CASE("Accepted MKW settings replace visible global preferences", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    Settings::SetMarioKartWorldRecommendedSettings(*settings, true);
    CheckPreset(*settings);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->gpu_accuracy.UsingGlobal());
    CHECK(settings->renderer_backend.GetValue(true) == Settings::RendererBackend::Vulkan);
    CHECK(settings->gpu_accuracy.GetValue(true) == Settings::GpuAccuracy::Low);
}

TEST_CASE("MKW preset overrides saved settings for the session", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    SetConflictingPreferences(*settings, true);
    REQUIRE(
        Settings::ApplyMarioKartWorldPreset(*settings, Settings::MarioKartWorldProgramId, true));
    CheckPreset(*settings);
    CheckPreferences(*settings, true);
    for (const auto& restore : settings->linkage.restore_functions) {
        restore();
    }
    CheckPreferences(*settings);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->resolution_setup.UsingGlobal());
    REQUIRE(
        Settings::ApplyMarioKartWorldPreset(*settings, Settings::MarioKartWorldProgramId, true));
    CheckPreset(*settings);
}

TEST_CASE("MKW preset does not change unrelated games", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    SetConflictingPreferences(*settings, false);
    CHECK_FALSE(Settings::ApplyMarioKartWorldPreset(*settings, 0, true));
    CHECK_FALSE(Settings::ApplyMarioKartWorldPreset(*settings,
                                                    Settings::MarioKartWorldProgramId + 1, true));
    CheckPreferences(*settings);
    CHECK(settings->renderer_backend.UsingGlobal());
    CHECK(settings->resolution_setup.UsingGlobal());
}

TEST_CASE("Declining the MKW preset preserves global and per-game preferences", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    for (bool per_game : {false, true}) {
        SetConflictingPreferences(*settings, per_game);
        CHECK_FALSE(Settings::ApplyMarioKartWorldPreset(*settings,
                                                        Settings::MarioKartWorldProgramId, false));
        CheckPreferences(*settings);
        CHECK(settings->renderer_backend.UsingGlobal() == !per_game);
        CHECK(settings->resolution_setup.UsingGlobal() == !per_game);
    }
}
