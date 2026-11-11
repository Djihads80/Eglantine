// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fmt/format.h>

#include "common/fs/fs.h"
#include "common/game_presets.h"
#include "video_core/renderer_vulkan/pipeline_cache_dir.h"

namespace Vulkan {

std::filesystem::path CreatePipelineCacheDir(const std::filesystem::path& shader_dir,
                                             u64 title_id) {
    auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (title_id == Settings::MarioKartWorldProgramId) {
        base_dir /= "mkw_zero_depth_v1";
    }
    if (!Common::FS::CreateDirs(base_dir)) {
        return {};
    }
    return base_dir;
}

} // namespace Vulkan
