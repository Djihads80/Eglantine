// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace Vulkan {

constexpr u64 LimitBudgetForHostCommit(u64 device_budget, u64 device_usage,
                                       u64 available_commit, u64 reserve) {
    if (available_commit >= reserve) {
        const u64 headroom = available_commit - reserve;
        if (device_usage >= device_budget || headroom >= device_budget - device_usage) {
            return device_budget;
        }
        return device_usage + headroom;
    }
    const u64 shortage = reserve - available_commit;
    return shortage >= device_usage ? 0 : (std::min)(device_budget, device_usage - shortage);
}

} // namespace Vulkan
