// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <set>
#include <string_view>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

} // namespace

std::optional<Nx2VideoConfiguration> ParseNx2VideoConfiguration(std::span<const u8> configuration) {
    if (configuration.empty()) {
        return std::nullopt;
    }
    const auto name_end = std::find(configuration.begin(), configuration.end(), u8{});
    const std::string_view mime{reinterpret_cast<const char*>(configuration.data()),
                               static_cast<size_t>(name_end - configuration.begin())};
    if (const auto codec = CodecFromMime(mime)) {
        return Nx2VideoConfiguration{*codec, {}};
    }
    const auto prefix_size = [&](size_t offset) -> size_t {
        const auto remaining = configuration.subspan(offset);
        if (remaining.size() >= 4 && remaining[0] == 0 && remaining[1] == 0 &&
            remaining[2] == 0 && remaining[3] == 1) {
            return 4;
        }
        if (remaining.size() >= 3 && remaining[0] == 0 && remaining[1] == 0 && remaining[2] == 1) {
            return 3;
        }
        return 0;
    };
    bool has_sps{};
    bool has_pps{};
    for (size_t offset = 0; offset < configuration.size();) {
        const size_t prefix = prefix_size(offset);
        if (prefix == 0 || offset + prefix == configuration.size()) {
            return std::nullopt;
        }
        const size_t start = offset + prefix;
        size_t end = start + 1;
        while (end < configuration.size() && prefix_size(end) == 0) {
            ++end;
        }
        const u8 header = configuration[start];
        const u8 type = header & 0x1f;
        if ((header & 0x80) != 0 || (type != 7 && type != 8) ||
            end - start < (type == 7 ? 4U : 2U)) {
            return std::nullopt;
        }
        has_sps |= type == 7;
        has_pps |= type == 8;
        offset = end;
    }
    if (!has_sps || !has_pps) {
        return std::nullopt;
    }
    return Nx2VideoConfiguration{NvdecCommon::VideoCodec::H264, configuration};
}

struct Nx2VideoDecoder::Impl {
    std::vector<u8> configuration;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    bool open{};
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data

    ~Impl() {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::span<const u8> configuration)
    : impl{std::make_unique<Impl>()} {
    impl->configuration.assign(configuration.begin(), configuration.end());
    const auto parsed = ParseNx2VideoConfiguration(impl->configuration);
    if (!parsed) {
        LOG_ERROR(HW_GPU, "nx2 video: unsupported {}-byte codec configuration", configuration.size());
        return;
    }
    impl->decoder.emplace(parsed->codec);
    if (impl->decoder->GetCodec() == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for codec {}",
                  static_cast<u64>(parsed->codec));
        return;
    }
    impl->context.emplace(*impl->decoder);
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        impl->hardware.emplace();
        impl->hardware->InitializeForDecoder(*impl->context, *impl->decoder);
    }
    impl->open = impl->context->OpenContext(*impl->decoder, parsed->extradata);
    LOG_INFO(HW_GPU, "nx2 video: decoder for codec {} ({} configuration bytes) {}",
             static_cast<u64>(parsed->codec), parsed->extradata.size(),
             impl->open ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

std::span<const u8> Nx2VideoDecoder::Configuration() const {
    return impl->configuration;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());
    FFmpeg::Packet packet{std::span<const u8>{impl->packet.data(), data.size()}};
    packet.GetPacket()->pts = static_cast<s64>(pts);
    // Hidden VP9 frames never come out: keep the set from growing over a long movie.
    if (impl->pending_pts.size() >= 64) {
        impl->pending_pts.erase(impl->pending_pts.begin());
    }
    impl->pending_pts.insert(pts);
    impl->context->SendPacket(packet);
    impl->Receive(pictures);
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    if (width == 0 || height == 0 || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    if (format != Nx2PictureFormat::Nv12) {
        // The stream's matrix and range; untagged HD video is BT.709.
        int space = SWS_CS_ITU601;
        if (f->colorspace == AVCOL_SPC_BT709 ||
            (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
            space = SWS_CS_ITU709;
        } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
            space = SWS_CS_BT2020;
        }
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(impl->scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
    }
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

} // namespace Tegra::Host1x
