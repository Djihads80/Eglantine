// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace Vulkan {

struct FramebufferSize {
    u32 width;
    u32 height;
    u32 layers;
};

// Draws without attachments take their size from the surface clip, which games leave at
// 32768x32768. Apple GPUs allow 16384, and Metal loses the device on a larger framebuffer.
constexpr FramebufferSize ClampFramebufferSize(FramebufferSize size, FramebufferSize max) {
    return {
        (std::max)(1U, (std::min)(size.width, max.width)),
        (std::max)(1U, (std::min)(size.height, max.height)),
        (std::max)(1U, (std::min)(size.layers, max.layers)),
    };
}

} // namespace Vulkan
