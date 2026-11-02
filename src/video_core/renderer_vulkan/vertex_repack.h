// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include "video_core/engines/maxwell_3d.h"

namespace Vulkan {

constexpr bool VertexRepackInputSupported(bool dynamic_input, bool experimental_static) {
    return dynamic_input || experimental_static;
}

// Pipelines without dynamic vertex input repack when the device cannot fetch the 69-byte stride.
// EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK=1 forces the repack on, =0 forces it off.
inline bool StaticVertexRepackSelected(const char* override_value, u32 stride_alignment) {
    if (override_value && std::strcmp(override_value, "1") == 0) {
        return true;
    }
    if (override_value && std::strcmp(override_value, "0") == 0) {
        return false;
    }
    return stride_alignment > 1 && 69 % stride_alignment != 0;
}

inline bool StaticVertexRepackEnabled(u32 stride_alignment) {
    return StaticVertexRepackSelected(std::getenv("EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK"),
                                      stride_alignment);
}

struct VertexRepackParameters {
    u32 vertices;
    u32 output_size;
    u32 source_base;
    u32 source_bias;
    u32 source_range;
    u32 dispatch_groups;
};

inline std::optional<VertexRepackParameters> MakeVertexRepackParameters(u32 offset, u32 size,
                                                                        u64 alignment,
                                                                        u64 max_storage_range,
                                                                        u32 max_dispatch_groups,
                                                                        u64 buffer_size) {
    if (size == 0 || alignment == 0) {
        return std::nullopt;
    }
    const u64 vertices = (u64{size} + 68) / 69;
    const u64 output_size = vertices * 72;
    const u64 base = u64{offset} / alignment * alignment;
    const u64 bias = offset - base;
    // The word-rounded range must not run past the end of the source buffer
    const u64 available = buffer_size > base ? buffer_size - base : 0;
    const u64 range = (std::min)((bias + size + 3) / 4 * 4, available);
    const u64 groups = (vertices * 18 + 127) / 128;
    if (range < bias + size || output_size > std::numeric_limits<u32>::max() ||
        range > std::numeric_limits<u32>::max() || output_size > max_storage_range ||
        range > max_storage_range || groups > max_dispatch_groups) {
        return std::nullopt;
    }
    return VertexRepackParameters{static_cast<u32>(vertices), static_cast<u32>(output_size),
                                 static_cast<u32>(base), static_cast<u32>(bias),
                                 static_cast<u32>(range), static_cast<u32>(groups)};
}

inline bool VertexRepackEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EGLANTINE_TEST_VERTEX_REPACK");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
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
