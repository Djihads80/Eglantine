// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "common/math_util.h"

namespace Service {

inline Common::Rectangle<int> Nx2PresentCrop(u32 origin, u32 end, u32 flags, u32 width,
                                            u32 height) {
    const Common::Rectangle<int> full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    if ((flags & 2) == 0) {
        return full;
    }
    const u32 left = std::min(origin & 0xffff, width);
    const u32 top = std::min(origin >> 16, height);
    const u32 right = std::min((end & 0xffff) + 1, width);
    const u32 bottom = std::min((end >> 16) + 1, height);
    if (left >= right || top >= bottom) {
        return full;
    }
    return {static_cast<int>(left), static_cast<int>(top), static_cast<int>(right),
            static_cast<int>(bottom)};
}

} // namespace Service
