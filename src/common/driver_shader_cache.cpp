// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/driver_shader_cache.h"
#include "common/fs/path_util.h"

namespace Common {
namespace {

std::string description{"driver shader cache settings unchanged"};
/// The private folder ConfigureDriverShaderCache pointed the driver at, until it is created.
std::filesystem::path pending_folder;
/// The __GL_SHADER_DISK_CACHE_SIZE it replaced, restored when the folder can't be created.
std::optional<std::string> replaced_size;

#if defined(__linux__) && !defined(__ANDROID__)
bool IsFalse(const char* value) {
    return value != nullptr && (value[0] == '0' || value[0] == 'f' || value[0] == 'F' ||
                                value[0] == 'n' || value[0] == 'N');
}
#endif

} // Anonymous namespace

void ConfigureDriverShaderCache([[maybe_unused]] bool enabled,
                                [[maybe_unused]] unsigned size_gib) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (!enabled) {
        description = "NVIDIA driver shader cache left shared (turned off in the settings)";
        return;
    }
    if (IsFalse(std::getenv("EDEN_NX2_PRIVATE_DRIVER_CACHE"))) {
        description = "NVIDIA driver shader cache left shared (EDEN_NX2_PRIVATE_DRIVER_CACHE=0)";
        return;
    }
    if (const char* path = std::getenv("__GL_SHADER_DISK_CACHE_PATH")) {
        description =
            fmt::format("NVIDIA driver shader cache in {} (set in the environment)", path);
        return;
    }
    if (IsFalse(std::getenv("__GL_SHADER_DISK_CACHE"))) {
        description = "NVIDIA driver shader cache turned off by the user";
        return;
    }
    // The folder is created later (CreateDriverShaderCacheFolder): creating it now would make
    // the user data migration find a cache folder in the way.
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "nvidia";
    setenv("__GL_SHADER_DISK_CACHE_PATH", path.c_str(), 1);
    pending_folder = path;
    // A size in the environment is the user's limit for the shared cache, not for this folder.
    std::string notes;
    if (const char* const environment_size = std::getenv("__GL_SHADER_DISK_CACHE_SIZE")) {
        replaced_size = environment_size;
        notes = fmt::format(" (instead of __GL_SHADER_DISK_CACHE_SIZE={})", environment_size);
    }
    const std::string size = std::to_string(u64{size_gib} << 30);
    setenv("__GL_SHADER_DISK_CACHE_SIZE", size.c_str(), 1);
    // Steam sets this for its own per-game caches; here it would turn the limit off.
    if (std::getenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP") != nullptr) {
        unsetenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP");
        notes += ", __GL_SHADER_DISK_CACHE_SKIP_CLEANUP ignored";
    }
    description = fmt::format("NVIDIA driver shader cache in {}, limit {} GiB{}", path.string(),
                              size_gib, notes);
#endif
}

void CreateDriverShaderCacheFolder() {
#if defined(__linux__) && !defined(__ANDROID__)
    if (pending_folder.empty()) {
        return;
    }
    const auto path = std::exchange(pending_folder, {});
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (!error) {
        return;
    }
    description = fmt::format("NVIDIA driver shader cache left shared: can't create {}: {}",
                              path.string(), error.message());
    unsetenv("__GL_SHADER_DISK_CACHE_PATH");
    if (replaced_size) {
        setenv("__GL_SHADER_DISK_CACHE_SIZE", replaced_size->c_str(), 1);
    } else {
        unsetenv("__GL_SHADER_DISK_CACHE_SIZE");
    }
#endif
}

std::string DriverShaderCacheDescription() {
    return description;
}

} // namespace Common
