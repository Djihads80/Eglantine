// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "video_core/memory_manager.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::Nx2Diag {

// Metadata only, requested by an inspected draw/dispatch during a one-frame capture. These are
// descriptor candidates in a bounded constant-buffer window, not proof the shader accesses them.
// No texture contents, cache flush or GPU readback is requested here. Caller supplies the live TIC
// pool for this draw/dispatch, so a later pool change cannot silently relabel an earlier resource.
inline std::string TextureDescriptorCandidates(Tegra::MemoryManager& memory, std::string_view stage,
                                              u32 slot, GPUVAddr buffer_address, u32 buffer_size,
                                              u32 first_offset, u32 requested_words,
                                              GPUVAddr tic_pool, u32 tic_limit) {
    constexpr u32 MaxWords = 64;
    if (first_offset >= buffer_size || first_offset % sizeof(u32) != 0 ||
        buffer_address > std::numeric_limits<GPUVAddr>::max() - first_offset) {
        return {};
    }
    const u32 count = std::min({requested_words, MaxWords, (buffer_size - first_offset) / 4});
    if (count == 0 || buffer_address + first_offset >
                          std::numeric_limits<GPUVAddr>::max() - count * sizeof(u32) ||
        !memory.IsFullyMappedRange(buffer_address + first_offset, count * 4)) {
        return {};
    }
    std::array<u32, MaxWords> words{};
    memory.ReadBlockUnsafe(buffer_address + first_offset, words.data(), count * 4);
    std::string result;
    for (u32 index = 0; index < count; ++index) {
        const u32 handle = words[index];
        const u32 tic_index = handle & 0xfffff;
        const u64 descriptor_offset = u64{tic_index} * sizeof(Tegra::Texture::TICEntry);
        if (handle == 0 || handle == ~0U || tic_index > tic_limit ||
            tic_pool > std::numeric_limits<GPUVAddr>::max() - descriptor_offset) {
            continue;
        }
        const GPUVAddr descriptor_address = tic_pool + descriptor_offset;
        Tegra::Texture::TICEntry tic{};
        if (descriptor_address > std::numeric_limits<GPUVAddr>::max() - sizeof(tic) ||
            !memory.IsFullyMappedRange(descriptor_address, sizeof(tic))) {
            continue;
        }
        memory.ReadBlockUnsafe(descriptor_address, &tic, sizeof(tic));
        if (!tic.IsBlockLinear() && !tic.IsPitchLinear() && !tic.IsBuffer()) {
            continue;
        }
        if (!result.empty()) {
            result += ',';
        }
        result += fmt::format(
            "{{\"stage\":\"{}\",\"cb\":{},\"offset\":{},\"handle\":\"{:#x}\","
            "\"tic\":{},\"descriptor\":\"{:#x}\",\"address\":\"{:#x}\","
            "\"width\":{},\"height\":{},\"depth\":{},\"format\":{},\"types\":[{},{},{},{}],"
            "\"swizzle\":[{},{},{},{}],\"srgb\":{},\"header\":{},\"block\":[{},{},{}],"
            "\"pitch\":{},\"base_layer\":{},\"mips\":[{},{}]}}",
            stage, slot, first_offset + index * 4, handle, tic_index, descriptor_address,
            tic.Address(), tic.Width(), tic.Height(), tic.Depth(),
            static_cast<u32>(tic.format.Value()), static_cast<u32>(tic.r_type.Value()),
            static_cast<u32>(tic.g_type.Value()), static_cast<u32>(tic.b_type.Value()),
            static_cast<u32>(tic.a_type.Value()), static_cast<u32>(tic.x_source.Value()),
            static_cast<u32>(tic.y_source.Value()), static_cast<u32>(tic.z_source.Value()),
            static_cast<u32>(tic.w_source.Value()), tic.srgb_conversion.Value(),
            static_cast<u32>(tic.header_version.Value()), tic.block_width.Value(),
            tic.block_height.Value(), tic.block_depth.Value(), tic.IsPitchLinear() ? tic.Pitch() : 0,
            tic.BaseLayer(), tic.res_min_mip_level.Value(), tic.res_max_mip_level.Value());
    }
    return result;
}

} // namespace VideoCommon::Nx2Diag
