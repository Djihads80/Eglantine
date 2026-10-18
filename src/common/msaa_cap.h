// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>

#include "common/common_types.h"

namespace Common {

/// Highest MSAA sample count the host device supports for attachments and sampled images.
/// Guest images keep their sample layout in memory; host images and rasterization are clamped.
inline std::atomic<u32> host_msaa_sample_cap{16};

inline void SetHostMsaaSampleCap(u32 samples) {
    host_msaa_sample_cap.store(samples, std::memory_order_relaxed);
}

[[nodiscard]] inline u32 HostMsaaSampleCap() {
    return host_msaa_sample_cap.load(std::memory_order_relaxed);
}

[[nodiscard]] inline u32 HostMsaaSamples(u32 guest_samples) {
    return (std::min)(guest_samples, HostMsaaSampleCap());
}

} // namespace Common
