// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "video_core/renderer_vulkan/pipeline_cache_dir.h"

namespace Vulkan {

std::filesystem::path CreatePipelineCacheDir(const std::filesystem::path& shader_dir,
                                             u64 title_id) {
    const auto base_dir{Common::FS::GetVulkanPipelineCacheDir(shader_dir, title_id)};
    if (!Common::FS::CreateDirs(base_dir)) {
        return {};
    }
    return base_dir;
}

} // namespace Vulkan
