// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/host1x/nvdec_common.h"

namespace FFmpeg {
class Frame;
}

namespace Tegra::Host1x {

struct Nx2VideoConfiguration {
    NvdecCommon::VideoCodec codec;
    std::span<const u8> extradata;
};

std::optional<Nx2VideoConfiguration> ParseNx2VideoConfiguration(std::span<const u8> configuration);

/// A picture decoded by Nx2VideoDecoder, in display order.
struct Nx2Picture {
    u32 width{};
    u32 height{};
    u64 pts{}; ///< As submitted with the compressed frame
    std::shared_ptr<FFmpeg::Frame> frame;
};

/// Pixel layouts of a Switch 2 decoder output image.
enum class Nx2PictureFormat : u32 {
    Nv12,  ///< Luma rows, then interleaved Cb/Cr rows at chroma_offset
    Rgba8,
    Bgra8,
};

/// The decoder behind the Switch 2 video decoder service (nn::codec::video): FFmpeg decodes the
/// elementary stream the game demuxed, in software or on the host GPU (NVDEC emulation setting).
/// Used from one thread at a time.
class Nx2VideoDecoder {
public:
    explicit Nx2VideoDecoder(std::span<const u8> configuration);
    ~Nx2VideoDecoder();

    bool IsOpen() const;

    std::span<const u8> Configuration() const;

    /// Decodes one compressed frame and appends the pictures it completes.
    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures);

    /// End of stream: appends the pictures the decoder still holds, then starts over.
    void Drain(std::vector<Nx2Picture>& pictures);

    /// Drops everything queued in the decoder (the game flushed it to seek).
    void Reset();

    /// Converts a picture to `format` in `out`: rows `pitch` bytes apart, NV12's chroma plane at
    /// `chroma_offset`, scaled to width x height.
    void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                u64 pitch, u64 chroma_offset, std::span<u8> out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Tegra::Host1x
