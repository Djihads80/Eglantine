// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "core/hle/service/nvnflinger/buffer_transform_flags.h"

namespace Service {

// Native present bit 18 requests vertical inversion (observed in Kirby's edition program).
inline android::BufferTransformFlags Nx2PresentTransform(u32 flags) {
    return (flags & (1U << 18)) != 0 ? android::BufferTransformFlags::FlipV
                                    : android::BufferTransformFlags::Unset;
}

} // namespace Service
