// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <fmt/format.h>

#include "common/driver_shader_cache.h"
#include "common/fs/path_util.h"

namespace Common {
namespace {

std::string description{"driver shader cache settings unchanged"};

#if defined(__linux__) && !defined(__ANDROID__)
// Holds about 400k pipelines at the ~26 KB NVIDIA stores for one.
constexpr const char* DefaultCacheSize = "10737418240";

bool IsFalse(const char* value) {
    return value != nullptr && (value[0] == '0' || value[0] == 'f' || value[0] == 'F' ||
                                value[0] == 'n' || value[0] == 'N');
}
#endif

} // Anonymous namespace

void ConfigureDriverShaderCache() {
#if defined(__linux__) && !defined(__ANDROID__)
    if (IsFalse(std::getenv("EDEN_NX2_PRIVATE_DRIVER_CACHE"))) {
        description = "NVIDIA driver shader cache left shared (EDEN_NX2_PRIVATE_DRIVER_CACHE=0)";
        return;
    }
    if (const char* path = std::getenv("__GL_SHADER_DISK_CACHE_PATH")) {
        description = fmt::format("NVIDIA driver shader cache in {} (set by the user)", path);
        return;
    }
    if (IsFalse(std::getenv("__GL_SHADER_DISK_CACHE"))) {
        description = "NVIDIA driver shader cache turned off by the user";
        return;
    }
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "nvidia";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        description = fmt::format("NVIDIA driver shader cache left shared: can't create {}: {}",
                                  path.string(), error.message());
        return;
    }
    setenv("__GL_SHADER_DISK_CACHE_PATH", path.c_str(), 1);
    const char* const user_size = std::getenv("__GL_SHADER_DISK_CACHE_SIZE");
    if (user_size == nullptr) {
        setenv("__GL_SHADER_DISK_CACHE_SIZE", DefaultCacheSize, 1);
    }
    description = fmt::format("NVIDIA driver shader cache in {}, limit {} bytes{}", path.string(),
                              user_size != nullptr ? user_size : DefaultCacheSize,
                              user_size != nullptr ? " (set by the user)" : "");
#endif
}

std::string DriverShaderCacheDescription() {
    return description;
}

} // namespace Common
