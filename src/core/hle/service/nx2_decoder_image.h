// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/host1x/nx2_video_decoder.h"

namespace Service {

struct Nx2DecoderImageLayout {
    u32 code;
    u32 width;
    u32 height;
    Tegra::Host1x::Nx2PictureFormat format;
    u64 pitch;
    u64 chroma_offset;
    u64 size;
    u32 gob_height_log2;
    bool block_linear;
};

inline Nx2DecoderImageLayout Nx2CalculateDecoderImageLayout(const u32* preference) {
    using Tegra::Host1x::Nx2PictureFormat;
    Nx2DecoderImageLayout layout{};
    layout.code = preference[1];
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    switch (layout.code) {
    case 1:
    case 4:
        layout.format = Nx2PictureFormat::Nv12;
        break;
    case 2:
    case 5:
        layout.format = Nx2PictureFormat::Rgba8;
        break;
    default:
        layout.format = Nx2PictureFormat::Rgba8;
        break;
    }
    const bool nv12 = layout.format == Nx2PictureFormat::Nv12;
    layout.block_linear = layout.code == 2 || layout.code == 5;
    layout.pitch = Common::AlignUp(u64{layout.width} * (nv12 ? 1 : 4), 64);
    if (layout.block_linear) {
        const u32 height_bits = std::bit_width(layout.height + (layout.height >> 1));
        layout.gob_height_log2 = height_bits > 4 ? std::min(height_bits - 4, 4U) : 0;
        layout.size = layout.pitch * Common::AlignUp(u64{layout.height},
                                                   u64{8} << layout.gob_height_log2);
    } else {
        layout.chroma_offset = layout.pitch * Common::AlignUp(u64{layout.height}, 128);
        layout.size = layout.chroma_offset +
                      (nv12 ? layout.pitch * Common::AlignUp((u64{layout.height} + 1) / 2, 128)
                            : 0);
    }
    return layout;
}

inline std::array<u32, 32> Nx2DecoderCpuImageDescriptor(const Nx2DecoderImageLayout& layout,
                                                      u64 offset) {
    using Tegra::Host1x::Nx2PictureFormat;
    std::array<u32, 32> words{};
    words[2] = static_cast<u32>(offset);
    if (layout.block_linear) {
        words[0] = 11;
        words[4] = static_cast<u32>(layout.pitch / 64);
        words[5] = 16 | ((8U << layout.gob_height_log2) << 16);
    } else {
        words[0] = layout.format == Nx2PictureFormat::Nv12 ? 10 : 8;
        words[4] = static_cast<u32>(layout.pitch);
        if (layout.format == Nx2PictureFormat::Nv12) {
            words[6] = static_cast<u32>(offset + layout.chroma_offset);
            words[8] = static_cast<u32>(layout.pitch);
        }
    }
    return words;
}

} // namespace Service
