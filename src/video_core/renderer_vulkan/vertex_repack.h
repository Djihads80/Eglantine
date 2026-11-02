// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include "video_core/engines/maxwell_3d.h"

namespace Vulkan {

constexpr bool VertexRepackInputSupported(bool dynamic_input, bool experimental_static) {
    return dynamic_input || experimental_static;
}

inline bool ExperimentalStaticVertexRepackEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct VertexRepackParameters {
    u32 vertices;
    u32 output_size;
    u32 source_base;
    u32 source_bias;
    u32 source_range;
    u32 dispatch_groups;
};

inline std::optional<VertexRepackParameters> MakeVertexRepackParameters(
    u32 offset, u32 size, u64 alignment, u64 max_storage_range, u32 max_dispatch_groups) {
    if (size == 0 || alignment == 0) {
        return std::nullopt;
    }
    const u64 vertices = (u64{size} + 68) / 69;
    const u64 output_size = vertices * 72;
    const u64 base = u64{offset} / alignment * alignment;
    const u64 bias = offset - base;
    const u64 range = (bias + size + 3) / 4 * 4;
    const u64 groups = (vertices * 18 + 127) / 128;
    if (output_size > std::numeric_limits<u32>::max() ||
        range > std::numeric_limits<u32>::max() || output_size > max_storage_range ||
        range > max_storage_range || groups > max_dispatch_groups) {
        return std::nullopt;
    }
    return VertexRepackParameters{static_cast<u32>(vertices), static_cast<u32>(output_size),
                                 static_cast<u32>(base), static_cast<u32>(bias),
                                 static_cast<u32>(range), static_cast<u32>(groups)};
}

// EGLANTINE_TEST_VERTEX_REPACK: "0" turns the repack off, "1" on for every driver; unset, it is on
// except where the device fetches the unaligned layout correctly (VertexRepackAllowed).
inline const char* VertexRepackSetting() {
    static const char* const value = std::getenv("EGLANTINE_TEST_VERTEX_REPACK");
    return value;
}

inline bool& VertexRepackAllowed() {
    static bool allowed = true;
    return allowed;
}

inline bool VertexRepackEnabled() {
    const char* value = VertexRepackSetting();
    if (value && std::strcmp(value, "0") == 0) {
        return false;
    }
    return VertexRepackAllowed() || (value && std::strcmp(value, "1") == 0);
}

inline u32 VertexRepackMask(const Tegra::Engines::Maxwell3D::Regs& regs) {
    using Attribute = Tegra::Engines::Maxwell3D::Regs::VertexAttribute;
    if (!VertexRepackEnabled()) {
        return 0;
    }
    u32 candidates = 0;
    u32 excluded = 0;
    for (const auto& attribute : regs.vertex_attrib_format) {
        const u32 binding = attribute.buffer;
        if (attribute.constant || regs.vertex_streams[binding].stride != 69) {
            continue;
        }
        if (attribute.size == Attribute::Size::Invalid) {
            excluded |= 1U << binding;
            continue;
        }
        const u32 offset = attribute.offset;
        const u32 size = attribute.SizeInBytes();
        if (offset == 65 && size == 4) {
            candidates |= 1U << binding;
        }
        if (offset + size > 69 || (offset < 65 && offset + size > 65)) {
            excluded |= 1U << binding;
        }
    }
    return candidates & ~excluded;
}

inline u32 RepackedVertexOffset(u32 mask, u32 binding, u32 offset) {
    return offset + ((mask & (1U << binding)) && offset >= 65 ? 3U : 0U);
}

inline u32 RepackedVertexStride(u32 mask, u32 binding, u32 stride) {
    return mask & (1U << binding) ? 72U : stride;
}

} // namespace Vulkan
