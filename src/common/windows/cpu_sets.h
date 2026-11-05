// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Common::Windows {

struct CpuSet {
    u32 id;
    u16 group;
    u8 logical_processor;
    u8 efficiency_class;
    bool allocated_elsewhere;
};

// Higher EfficiencyClass values identify faster cores. Honor existing constraints before
// comparing classes; a homogeneous allowed set needs no additional placement policy.
inline std::vector<u32> SelectPerformanceCpuSets(std::span<const CpuSet> topology, u16 group,
                                                u64 allowed_mask,
                                                std::span<const u32> process_defaults = {}) {
    const auto allowed = [&](const CpuSet& cpu) {
        return cpu.group == group && cpu.logical_processor < 64 &&
               (allowed_mask & (u64{1} << cpu.logical_processor)) != 0 &&
               !cpu.allocated_elsewhere &&
               (process_defaults.empty() ||
                std::find(process_defaults.begin(), process_defaults.end(), cpu.id) !=
                    process_defaults.end());
    };
    u8 lowest = 255;
    u8 highest = 0;
    for (const auto& cpu : topology) {
        if (allowed(cpu)) {
            lowest = (std::min)(lowest, cpu.efficiency_class);
            highest = (std::max)(highest, cpu.efficiency_class);
        }
    }
    std::vector<u32> selected;
    if (lowest >= highest) {
        return selected;
    }
    for (const auto& cpu : topology) {
        if (allowed(cpu) && cpu.efficiency_class == highest) {
            selected.push_back(cpu.id);
        }
    }
    return selected;
}

} // namespace Common::Windows
