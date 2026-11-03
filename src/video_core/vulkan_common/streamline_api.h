// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Streamline's API as loaded from the user's sl.interposer.dll. Only for the
// Streamline sources: it pulls in <windows.h> and Streamline's (MIT-licensed) headers.

#pragma once

#include <windows.h>

#include "video_core/vulkan_common/vulkan_wrapper.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>

namespace Vulkan::Streamline {

struct Api {
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* is_feature_supported{};
    PFun_slGetFeatureRequirements* get_feature_requirements{};
    PFun_slGetFeatureVersion* get_feature_version{};
    PFun_slGetFeatureFunction* get_feature_function{};
    PFun_slGetNewFrameToken* get_new_frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slEvaluateFeature* evaluate_feature{};
    PFun_slFreeResources* free_resources{};
    PFun_slSetVulkanInfo* set_vulkan_info{};
};

/// The loaded functions; only valid while IsDlssAvailable().
[[nodiscard]] const Api& GetApi();

} // namespace Vulkan::Streamline
