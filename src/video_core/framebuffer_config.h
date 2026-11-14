// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/math_util.h"
#include "core/hle/service/nvnflinger/buffer_transform_flags.h"
#include "core/hle/service/nvnflinger/hwc_layer.h"
#include "core/hle/service/nvnflinger/pixel_format.h"
#include "core/hle/service/nvnflinger/ui/fence.h"

namespace Tegra {

enum class BlendMode {
    Opaque,
    Premultiplied,
    Coverage,
};

enum class ColorSpace {
    RGB709, // Presented as sRGB.
    RGB2020_PQ,
    RGB709_Linear,
};

/**
 * Struct describing framebuffer configuration
 */
struct FramebufferConfig {
    DAddr address{};
    u32 offset{};
    u32 width{};
    u32 height{};
    u32 stride{};
    Service::android::PixelFormat pixel_format{};
    Service::android::BufferTransformFlags transform_flags{};
    Common::Rectangle<int> crop_rect{};
    BlendMode blending{};
    bool native_tiling{};
    u32 layer_stack_mask{Service::Nvnflinger::DefaultLayerStackMask};
    ColorSpace color_space{ColorSpace::RGB709};
    u16 linear_unit_luminance{}; // Nits per linear 1.0; typically 80 for scRGB (observed in MKW's SDK).
};

Common::Rectangle<f32> NormalizeCrop(const FramebufferConfig& framebuffer, u32 texture_width,
                                     u32 texture_height);

/**
 * Returns the subset of layers belonging to a stack.
 */
std::span<const FramebufferConfig> FilterLayerStack(std::span<const FramebufferConfig> layers,
                                                    Service::Nvnflinger::LayerStackId stack,
                                                    std::vector<FramebufferConfig>& scratch);

} // namespace Tegra
