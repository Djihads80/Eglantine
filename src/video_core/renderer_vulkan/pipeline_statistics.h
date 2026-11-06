// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

class PipelineStatistics {
public:
    explicit PipelineStatistics(const Device& device_);
    /// `label`: logged with each stage's statistics when they are wanted (Wanted).
    void Collect(const Device& device, VkPipeline pipeline, std::string_view label = {});
    void Report() const;

    /// EDEN_NX2_PIPELINE_STATS=1: the driver's statistics for every pipeline are captured and
    /// logged with its shader hashes (on Turnip: registers, waves, instructions, spills), without
    /// the renderer debug and shader feedback settings. Read once the game loads.
    [[nodiscard]] static bool Wanted();
    /// The collector for pipelines built while the game runs (Wanted only, else null).
    [[nodiscard]] static PipelineStatistics* ForRuntime(const Device& device);

private:
    struct Stats {
        u64 code_size{};
        u64 register_count{};
        u64 sgpr_count{};
        u64 vgpr_count{};
        u64 branches_count{};
        u64 basic_block_count{};
    };
    mutable std::mutex mutex;
    std::vector<Stats> collected_stats;
};

} // namespace Vulkan
