// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/game_presets.h"
#include "video_core/renderer_vulkan/pipeline_cache_dir.h"

namespace {

namespace fs = std::filesystem;
using Settings::MarioKartWorldProgramId;

// An empty shader directory, removed with everything in it at the end of the test.
struct ScratchShaderDir {
    fs::path path = fs::temp_directory_path() /
                    ("pipeline-cache-dir-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchShaderDir() {
        fs::create_directories(path);
    }
    ~ScratchShaderDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // Anonymous namespace

TEST_CASE("Pipeline cache directory is created for a title on a fresh cache",
          "[video_core][vulkan][pipeline_cache_dir]") {
    ScratchShaderDir shader;
    const auto dir = Vulkan::CreatePipelineCacheDir(shader.path, 0x0100000000010000ULL);
    CHECK(dir == shader.path / "0100000000010000");
    CHECK(fs::is_directory(dir));
}

TEST_CASE("Mario Kart World pipeline cache subdirectory is created on a fresh cache",
          "[video_core][vulkan][pipeline_cache_dir]") {
    ScratchShaderDir shader;
    const auto expected = shader.path / "0400c3f00006e000" / "mkw_zero_depth_v1";
    REQUIRE_FALSE(fs::exists(shader.path / "0400c3f00006e000"));
    CHECK(Vulkan::CreatePipelineCacheDir(shader.path, MarioKartWorldProgramId) == expected);
    CHECK(fs::is_directory(expected));
    // An existing cache directory is reused.
    CHECK(Vulkan::CreatePipelineCacheDir(shader.path, MarioKartWorldProgramId) == expected);
}

TEST_CASE("Pipeline cache directory creation fails when a file is in the way",
          "[video_core][vulkan][pipeline_cache_dir]") {
    ScratchShaderDir shader;
    std::ofstream{shader.path / "0400c3f00006e000"} << "not a directory";
    CHECK(Vulkan::CreatePipelineCacheDir(shader.path, MarioKartWorldProgramId).empty());
}
