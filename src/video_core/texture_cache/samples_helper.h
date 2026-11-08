// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

#include "common/assert.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

[[nodiscard]] inline std::pair<int, int> SamplesLog2(int num_samples) {
    switch (num_samples) {
    case 1:
        return {0, 0};
    case 2:
        return {1, 0};
    case 4:
        return {1, 1};
    case 8:
        return {2, 1};
    case 16:
        return {2, 2};
    }
    ASSERT_MSG(false, "Invalid number of samples={}", num_samples);
    return {0, 0};
}

/// The copy from the single-sample scratch image an MSAA upload lands in to the MSAA image.
/// The upload writes the scratch image at the copy's own offset, in samples; the MSAA image's
/// offset and extent are in pixels.
[[nodiscard]] inline ImageCopy MsaaUploadCopy(const BufferImageCopy& copy, int num_samples) {
    const auto [samples_x, samples_y] = SamplesLog2(num_samples);
    return ImageCopy{
        .src_subresource = copy.image_subresource,
        .dst_subresource = copy.image_subresource,
        .src_offset = copy.image_offset,
        .dst_offset = {copy.image_offset.x >> samples_x, copy.image_offset.y >> samples_y,
                       copy.image_offset.z},
        .extent = {copy.image_extent.width >> samples_x, copy.image_extent.height >> samples_y,
                   copy.image_extent.depth},
    };
}

[[nodiscard]] inline int NumSamples(Tegra::Texture::MsaaMode msaa_mode) {
    using Tegra::Texture::MsaaMode;
    switch (msaa_mode) {
    case MsaaMode::Msaa1x1:
        return 1;
    case MsaaMode::Msaa2x1:
    case MsaaMode::Msaa2x1_D3D:
        return 2;
    case MsaaMode::Msaa2x2:
    case MsaaMode::Msaa2x2_VC4:
    case MsaaMode::Msaa2x2_VC12:
        return 4;
    case MsaaMode::Msaa4x2:
    case MsaaMode::Msaa4x2_D3D:
    case MsaaMode::Msaa4x2_VC8:
    case MsaaMode::Msaa4x2_VC24:
        return 8;
    case MsaaMode::Msaa4x4:
        return 16;
    }
    ASSERT_MSG(false, "Invalid MSAA mode={}", static_cast<int>(msaa_mode));
    return 1;
}

[[nodiscard]] inline int NumSamplesX(Tegra::Texture::MsaaMode msaa_mode) {
    using Tegra::Texture::MsaaMode;
    switch (msaa_mode) {
    case MsaaMode::Msaa1x1:
        return 1;
    case MsaaMode::Msaa2x1:
    case MsaaMode::Msaa2x1_D3D:
    case MsaaMode::Msaa2x2:
    case MsaaMode::Msaa2x2_VC4:
    case MsaaMode::Msaa2x2_VC12:
        return 2;
    case MsaaMode::Msaa4x2:
    case MsaaMode::Msaa4x2_D3D:
    case MsaaMode::Msaa4x2_VC8:
    case MsaaMode::Msaa4x2_VC24:
    case MsaaMode::Msaa4x4:
        return 4;
    }
    ASSERT_MSG(false, "Invalid MSAA mode={}", static_cast<int>(msaa_mode));
    return 1;
}

[[nodiscard]] inline int NumSamplesY(Tegra::Texture::MsaaMode msaa_mode) {
    using Tegra::Texture::MsaaMode;
    switch (msaa_mode) {
    case MsaaMode::Msaa1x1:
    case MsaaMode::Msaa2x1:
    case MsaaMode::Msaa2x1_D3D:
        return 1;
    case MsaaMode::Msaa2x2:
    case MsaaMode::Msaa2x2_VC4:
    case MsaaMode::Msaa2x2_VC12:
    case MsaaMode::Msaa4x2:
    case MsaaMode::Msaa4x2_D3D:
    case MsaaMode::Msaa4x2_VC8:
    case MsaaMode::Msaa4x2_VC24:
        return 2;
    case MsaaMode::Msaa4x4:
        return 4;
    }
    ASSERT_MSG(false, "Invalid MSAA mode={}", static_cast<int>(msaa_mode));
    return 1;
}

} // namespace VideoCommon
