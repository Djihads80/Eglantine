// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using VideoCore::Surface::PixelFormat;

// Native descriptors address the selected view directly. The legacy NVN
// layer-base bookkeeping is not an additional array prefix for this ABI.
inline TICEntry TextureViewHeader(TICEntry entry, bool native) {
    if (native) {
        entry.layer_base_0_2.Assign(0);
        entry.layer_base_3_7.Assign(0);
        entry.layer_base_8_10.Assign(0);
    }
    return entry;
}

struct ImageInfo {
    ImageInfo() = default;
    explicit ImageInfo(const TICEntry& config) noexcept;
    explicit ImageInfo(const Tegra::Engines::Maxwell3D::Regs::RenderTargetConfig& ct,
                       Tegra::Texture::MsaaMode msaa_mode) noexcept;
    explicit ImageInfo(const Tegra::Engines::Maxwell3D::Regs::Zeta& zt,
                       const Tegra::Engines::Maxwell3D::Regs::ZetaSize& zt_size,
                       Tegra::Texture::MsaaMode msaa_mode) noexcept;
    explicit ImageInfo(const Tegra::Engines::Fermi2D::Surface& config) noexcept;
    explicit ImageInfo(const Tegra::DMA::ImageOperand& config) noexcept;

    /// Returns whether native memory tiling applies after the descriptor's legacy-layout override.
    [[nodiscard]] bool UsesNativeSectorLayout(bool native) const noexcept {
        return native && !legacy_sector_layout;
    }

    /// Accepts unknown layouts; otherwise requires the same effective native-tiling state.
    [[nodiscard]] bool IsSectorLayoutCompatible(const ImageInfo& other) const noexcept {
        return !sector_layout_known || !other.sector_layout_known ||
               native_tiling == other.native_tiling;
    }

    /// Takes the known sector layout of `other` when this one's is unknown; returns whether
    /// the guest memory order changed. Only for images without GPU-written data.
    bool AdoptSectorLayout(const ImageInfo& other) noexcept {
        if (sector_layout_known || !other.sector_layout_known) {
            return false;
        }
        const bool changed = native_tiling != other.native_tiling;
        native_tiling = other.native_tiling;
        legacy_sector_layout = other.legacy_sector_layout;
        sector_layout_known = true;
        return changed;
    }

    PixelFormat format = PixelFormat::Invalid;
    ImageType type = ImageType::e1D;
    SubresourceExtent resources;
    Extent3D size{1, 1, 1};
    union {
        Extent3D block{0, 0, 0};
        u32 pitch;
    };
    u32 layer_stride = 0;
    u32 maybe_unaligned_layer_stride = 0;
    u32 num_samples = 1;
    u32 tile_width_spacing = 0;
    bool native_tiling = false;
    // Native TIC header bit 88 selects the legacy Tegra sector order.
    bool legacy_sector_layout = false;
    // Render target registers do not specify the texture descriptor's sector order.
    bool sector_layout_known = false;
    bool rescaleable = false;
    bool downscaleable = false;
    bool forced_flushed = false;
    bool dma_downloaded = false;
    bool is_sparse = false;
    // ASTC data whose blocks carry no alpha: re-compressed to BC1 instead of BC3 (half the size).
    bool astc_opaque = false;
};

} // namespace VideoCommon
