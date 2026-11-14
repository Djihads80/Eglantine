// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>
#include <span>

#include "common/common_types.h"

namespace Common {

struct CpuCacheDomain {
    u64 mask;
    u64 bytes;
};

struct CpuThreadLayout {
    u64 foreground;
    u64 backend;
    u64 support;
    /// The foreground core next below the backend's: the split's front gets it alone with
    /// EDEN_NX2_CPU_LAYOUT=2 (it is still part of `foreground` here).
    u64 front{};
};

inline std::optional<CpuThreadLayout> MakeCpuThreadLayout(u64 allowed,
                                                          std::span<const CpuCacheDomain> caches,
                                                          std::span<const u64> cores) {
    if (caches.size() != 2 || allowed == 0 || caches[0].mask == 0 || caches[1].mask == 0 ||
        caches[0].bytes == 0 || caches[1].bytes == 0 || caches[0].bytes == caches[1].bytes ||
        (caches[0].mask & caches[1].mask) != 0 || (caches[0].mask | caches[1].mask) != allowed) {
        return std::nullopt;
    }
    const size_t primary = caches[0].bytes > caches[1].bytes ? 0 : 1;
    const u64 foreground = caches[primary].mask;
    const u64 support = caches[1 - primary].mask;
    u64 covered{};
    u64 backend{};
    u64 front{};
    size_t foreground_cores{};
    size_t support_cores{};
    for (const u64 mask : cores) {
        if (mask == 0 || (mask & covered) != 0 || (mask & ~allowed) != 0) {
            return std::nullopt;
        }
        covered |= mask;
        if ((mask & ~foreground) == 0) {
            ++foreground_cores;
            if (mask > backend) {
                front = backend;
                backend = mask;
            } else if (mask > front) {
                front = mask;
            }
        } else if ((mask & ~support) == 0) {
            ++support_cores;
        } else {
            return std::nullopt;
        }
    }
    // Six guest cores and draw preparation share the cache domain alongside the backend core.
    if (covered != allowed || foreground_cores < 8 || support_cores < 2) {
        return std::nullopt;
    }
    return CpuThreadLayout{foreground & ~backend, backend, support, front};
}

} // namespace Common
