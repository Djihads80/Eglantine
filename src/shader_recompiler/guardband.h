// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include "common/common_types.h"

namespace Shader {

// SET_VIEWPORT_CLIP_CONTROL: XY bit 7, Z bits 2:1 (same as XY, 256, or 1).
constexpr u32 WScaleFactorXY(u32 xy_mode) {
    return xy_mode == 0 ? 0x43800000U : 0x3f800000U;
}

constexpr std::optional<u32> WScaleFactorZ(u32 xy_mode, u32 z_mode) {
    switch (z_mode) {
    case 0: return WScaleFactorXY(xy_mode);
    case 1: return 0x43800000U;
    case 2: return 0x3f800000U;
    default: return std::nullopt;
    }
}

} // namespace Shader
