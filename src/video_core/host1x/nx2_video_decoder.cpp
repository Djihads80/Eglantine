// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <set>
#include <thread>

#include <gav1/decoder.h>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
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
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

AVPixelFormat Av1PixelFormat(const libgav1::DecoderBuffer& buffer) {
    if (buffer.bitdepth != 8 && buffer.bitdepth != 10) {
        return AV_PIX_FMT_NONE;
    }
    const bool ten_bit = buffer.bitdepth == 10;
    switch (buffer.image_format) {
    case kLibgav1ImageFormatYuv420:
        return ten_bit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    case kLibgav1ImageFormatYuv422:
        return ten_bit ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    case kLibgav1ImageFormatYuv444:
        return ten_bit ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
    case kLibgav1ImageFormatMonochrome400:
        return ten_bit ? AV_PIX_FMT_GRAY10LE : AV_PIX_FMT_GRAY8;
    }
    return AV_PIX_FMT_NONE;
}

// Copies a picture libgav1 decoded into a frame of its own (libgav1 reuses its buffers), tagged
// with the stream's matrix and range for Render.
std::shared_ptr<FFmpeg::Frame> CopyAv1Picture(const libgav1::DecoderBuffer& buffer) {
    const AVPixelFormat format = Av1PixelFormat(buffer);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    if (descriptor == nullptr || buffer.displayed_width[0] <= 0 ||
        buffer.displayed_height[0] <= 0) {
        return nullptr;
    }
    auto frame = std::make_shared<FFmpeg::Frame>();
    AVFrame* f = frame->GetFrame();
    f->format = format;
    f->width = buffer.displayed_width[0];
    f->height = buffer.displayed_height[0];
    if (av_frame_get_buffer(f, 0) < 0) {
        return nullptr;
    }
    const int sample_bytes = buffer.bitdepth > 8 ? 2 : 1;
    for (int plane = 0; plane < buffer.NumPlanes(); ++plane) {
        const int width =
            plane == 0 ? f->width : AV_CEIL_RSHIFT(f->width, descriptor->log2_chroma_w);
        const int height =
            plane == 0 ? f->height : AV_CEIL_RSHIFT(f->height, descriptor->log2_chroma_h);
        const size_t row_bytes =
            static_cast<size_t>(std::min(width, buffer.displayed_width[plane]) * sample_bytes);
        for (int y = 0; y < std::min(height, buffer.displayed_height[plane]); ++y) {
            std::memcpy(f->data[plane] + static_cast<ptrdiff_t>(y) * f->linesize[plane],
                        buffer.plane[plane] + static_cast<ptrdiff_t>(y) * buffer.stride[plane],
                        row_bytes);
        }
    }
    // AV1 numbers its matrices as FFmpeg does (ITU-T H.273).
    const int matrix = static_cast<int>(buffer.matrix_coefficients);
    f->colorspace = matrix <= static_cast<int>(AVCOL_SPC_ICTCP) ? static_cast<AVColorSpace>(matrix)
                                                                : AVCOL_SPC_UNSPECIFIED;
    f->color_range =
        buffer.color_range == kLibgav1ColorRangeFull ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    return frame;
}

// The stream's matrix and range for a conversion to RGB; untagged HD video is BT.709.
void SetRgbColorspace(SwsContext* scaler, const AVFrame* f) {
    int space = SWS_CS_ITU601;
    if (f->colorspace == AVCOL_SPC_BT709 ||
        (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
        space = SWS_CS_ITU709;
    } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
        space = SWS_CS_BT2020;
    }
    const int* coefficients = sws_getCoefficients(space);
    sws_setColorspaceDetails(scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG, coefficients,
                             1, 0, 1 << 16, 1 << 16);
}

} // namespace

bool IsH264Stream(std::span<const u8> data) {
    if (data.size() >= 4) {
        // Annex-B 3-byte start code: 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
            const u8 nal_type = data[3] & 0x1F;
            if ((data[3] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 5) {
        // Annex-B 4-byte start code: 00 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
        // avcC / MP4 length-prefixed NAL unit: [u32 be length] <nal_header>
        const u32 nalu_len = (static_cast<u32>(data[0]) << 24) |
                             (static_cast<u32>(data[1]) << 16) |
                             (static_cast<u32>(data[2]) << 8) |
                             static_cast<u32>(data[3]);
        if (nalu_len > 0 && nalu_len <= data.size() - 4) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 7) {
        // avcC configuration record: version=1, profile, compat, level, lengthSizeMinusOne | 0xFC, numSPS | 0xE0
        if (data[0] == 1 && (data[4] & 0xFC) == 0xFC && (data[5] & 0xE0) == 0xE0 && (data[5] & 0x1F) > 0) {
            return true;
        }
    }
    return false;
}

bool Nx2IsAv1Mime(std::string_view mime) {
    return mime.find("av01") != std::string_view::npos ||
           mime.find("av1") != std::string_view::npos;
}

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    std::unique_ptr<libgav1::Decoder> av1; ///< Instead of FFmpeg, for AV1
    u64 av1_errors{};
    SwsContext* scaler{};
    static constexpr size_t Bands = 4;
    std::array<SwsContext*, Bands> band_scalers{}; ///< RenderBands
    bool open{};
    H264PacketConverter h264_packets;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data

    ~Impl() {
        sws_freeContext(scaler);
        for (SwsContext* band_scaler : band_scalers) {
            sws_freeContext(band_scaler);
        }
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

    // Converts a frame to RGB at its own size in bands of rows, each with a context of its own, on
    // threads (fefw converts its AV1 frames to RGBA on four). Bands start at rows the chroma
    // subsampling divides, so no chroma row is split. Returns false if it can't.
    bool RenderBands(const AVFrame* f, AVPixelFormat target, u32 height, u64 pitch,
                     std::span<u8> out) {
        const auto source = static_cast<AVPixelFormat>(f->format);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(source);
        const int plane_count = av_pix_fmt_count_planes(source);
        if (descriptor == nullptr || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0 ||
            plane_count <= 0 || plane_count > 4) {
            return false;
        }
        const u32 row_alignment = 1U << std::max<u32>(1, descriptor->log2_chroma_h);
        std::array<u32, Bands + 1> rows{};
        for (size_t band = 0; band < Bands; ++band) {
            rows[band] = static_cast<u32>(u64{height} * band / Bands) & ~(row_alignment - 1);
        }
        rows[Bands] = height;
        for (size_t band = 0; band < Bands; ++band) {
            const int band_height = static_cast<int>(rows[band + 1] - rows[band]);
            band_scalers[band] = sws_getCachedContext(band_scalers[band], f->width, band_height,
                                                      source, f->width, band_height, target,
                                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (band_scalers[band] == nullptr) {
                return false;
            }
            SetRgbColorspace(band_scalers[band], f);
        }
        const auto convert = [&](size_t index) {
            std::array<const u8*, 4> source_planes{};
            for (int plane = 0; plane < plane_count; ++plane) {
                const bool chroma = plane == 1 || plane == 2;
                const u32 first_row =
                    chroma ? rows[index] >> descriptor->log2_chroma_h : rows[index];
                source_planes[plane] =
                    f->data[plane] + static_cast<ptrdiff_t>(first_row) * f->linesize[plane];
            }
            const std::array<u8*, 4> target_planes{out.data() + rows[index] * pitch, nullptr,
                                                   nullptr, nullptr};
            const std::array<int, 4> target_strides{static_cast<int>(pitch), 0, 0, 0};
            sws_scale(band_scalers[index], source_planes.data(), f->linesize, 0,
                      static_cast<int>(rows[index + 1] - rows[index]), target_planes.data(),
                      target_strides.data());
        };
        {
            std::array<std::jthread, Bands - 1> workers;
            for (size_t band = 1; band < Bands; ++band) {
                workers[band - 1] = std::jthread{convert, band};
            }
            convert(0);
        }
        return true;
    }

    // Not in frame parallel mode, libgav1 decodes one temporal unit per DequeueFrame and holds
    // no picture back, so each one is decoded right away (EnqueueFrame keeps a pointer to it).
    void DecodeAv1(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
        auto status =
            av1->EnqueueFrame(data.data(), data.size(), static_cast<int64_t>(pts), nullptr);
        const libgav1::DecoderBuffer* buffer{};
        if (status == libgav1::kStatusOk) {
            status = av1->DequeueFrame(&buffer);
        }
        if (status != libgav1::kStatusOk) {
            if (av1_errors++ < 8 || (av1_errors % 600) == 0) {
                LOG_ERROR(HW_GPU, "nx2 video: AV1 frame of {} bytes not decoded ({}), {} so far",
                          data.size(), libgav1::GetErrorString(status), av1_errors);
            }
            return;
        }
        if (buffer == nullptr) {
            return; // nothing shown by this temporal unit
        }
        auto frame = CopyAv1Picture(*buffer);
        if (!frame) {
            if (av1_errors++ < 8) {
                LOG_ERROR(HW_GPU, "nx2 video: unsupported AV1 picture: {}-bit, format {}, {}x{}",
                          buffer->bitdepth, static_cast<int>(buffer->image_format),
                          buffer->displayed_width[0], buffer->displayed_height[0]);
            }
            return;
        }
        pictures.push_back({static_cast<u32>(buffer->displayed_width[0]),
                            static_cast<u32>(buffer->displayed_height[0]),
                            static_cast<u64>(buffer->user_private_data), std::move(frame)});
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime) : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    // AV1 goes to libgav1 where FFmpeg has no libdav1d (Android's FFmpeg package); FFmpeg's own
    // "av1" decoder only drives hardware decoders.
    if (Nx2IsAv1Mime(mime) && avcodec_find_decoder_by_name("libdav1d") == nullptr) {
        auto av1 = std::make_unique<libgav1::Decoder>();
        libgav1::DecoderSettings settings;
        settings.threads =
            static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 4U));
        const auto status = av1->Init(&settings);
        impl->open = status == libgav1::kStatusOk;
        if (impl->open) {
            impl->av1 = std::move(av1);
        }
        LOG_INFO(HW_GPU, "nx2 video: libgav1 decoder for \"{}\" {} ({} threads)", mime,
                 impl->open ? "open" : libgav1::GetErrorString(status), settings.threads);
        return;
    }
    const auto codec = CodecFromMime(mime);
    if (!codec) {
        LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
        return;
    }
    impl->decoder.emplace(*codec);
    if (impl->decoder->GetCodec() == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
        return;
    }
    impl->context.emplace(*impl->decoder);
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        impl->hardware.emplace();
        impl->hardware->InitializeForDecoder(*impl->context, *impl->decoder);
    }
    impl->open = impl->context->OpenContext(*impl->decoder);
    LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" {}", mime, impl->open ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
    if (impl->av1) {
        impl->DecodeAv1(data, pts, pictures);
        return;
    }
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());

    // Convert MP4/avcC length-prefixed NALUs to Annex-B start codes for FFmpeg if needed.
    if (impl->name.find("avc") != std::string::npos || impl->name.find("264") != std::string::npos) {
        impl->h264_packets.Convert(std::span<u8>{impl->packet.data(), data.size()});
    }

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
    if (impl->av1) {
        Reset(); // libgav1 holds no picture back (see DecodeAv1)
        return;
    }
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
    if (impl->av1) {
        // Releases its frames; the next frame may start a new sequence.
        impl->av1->SignalEOS();
    } else if (impl->open) {
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
    // Frames copied back from a host GPU decoder are already NV12. Copy them as they are:
    // FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which shows
    // the movies green.
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
        static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                        width);
        }
        return;
    }
    // Large RGB images at the frame's own size convert on several threads (NV12 doesn't).
    if (format != Nx2PictureFormat::Nv12 && static_cast<u32>(f->width) == width &&
        static_cast<u32>(f->height) == height && height >= 256 &&
        impl->RenderBands(f, target, height, pitch, out)) {
        return;
    }
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
        SetRgbColorspace(impl->scaler, f);
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
