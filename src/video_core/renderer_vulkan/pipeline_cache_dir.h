// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "common/common_types.h"

namespace Vulkan {

/// Creates the title's pipeline cache directory under shader_dir.
/// Returns an empty path on failure.
[[nodiscard]] std::filesystem::path CreatePipelineCacheDir(const std::filesystem::path& shader_dir,
                                                           u64 title_id);

} // namespace Vulkan
