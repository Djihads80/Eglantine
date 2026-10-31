// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>

#include "common/common_types.h"
#include "core/device_memory_manager.h"

namespace VideoCommon {

template <typename Translate>
bool IsSamePhysicalRange(DAddr left, DAddr right, u64 size, Translate&& translate) {
    if (size == 0 || size > std::numeric_limits<u64>::max() - left ||
        size > std::numeric_limits<u64>::max() - right ||
        (left & Core::DEVICE_PAGEMASK) != (right & Core::DEVICE_PAGEMASK)) {
        return false;
    }
    while (size != 0) {
        const PAddr physical = translate(left);
        if (physical == 0 || physical != translate(right)) {
            return false;
        }
        const u64 step = std::min<u64>(size, Core::DEVICE_PAGESIZE - (left & Core::DEVICE_PAGEMASK));
        left += step;
        right += step;
        size -= step;
    }
    return true;
}

} // namespace VideoCommon
