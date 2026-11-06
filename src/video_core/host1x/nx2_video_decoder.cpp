// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <thread>

#ifdef EDEN_NX2_LIBGAV1
#include <gav1/decoder.h>
#endif

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

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

#ifdef EDEN_NX2_LIBGAV1
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
#endif

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

#ifdef __ANDROID__
// EDEN_NX2_VIDEO_MEDIACODEC=0 decodes AV1 in software (libgav1) instead of with MediaCodec. Read
// each time a decoder is created, so after env.txt.
bool MediaCodecAllowed() {
    const char* const value = std::getenv("EDEN_NX2_VIDEO_MEDIACODEC");
    return value == nullptr || value[0] != '0';
}

/// Decodes with the platform's MediaCodec into NV12 FFmpeg frames, so Render is unchanged: AV1,
/// which Android's FFmpeg package has no decoder for and libgav1 decodes too slowly for Ocarina
/// of Time's 1080p60 movies; most phones decode it in hardware. Adapted from pavelgajdo's
/// Lichii-Android 8bce98ae84. Hardware decoders finish frames on their own time: Decode queues a
/// frame and takes what is ready, Poll collects the rest. Used from one thread.
class MediaCodecDecoder {
public:
    static std::unique_ptr<MediaCodecDecoder> Create(const char* mime, u32 width, u32 height) {
        AMediaCodec* const codec = AMediaCodec_createDecoderByType(mime);
        if (codec == nullptr) {
            LOG_WARNING(HW_GPU, "nx2 video: MediaCodec has no decoder for \"{}\"", mime);
            return nullptr;
        }
        auto decoder = std::unique_ptr<MediaCodecDecoder>(new MediaCodecDecoder(codec));
        decoder->width = width != 0 ? static_cast<s32>(width) : 1920;
        decoder->height = height != 0 ? static_cast<s32>(height) : 1080;
        decoder->stride = decoder->width;
        decoder->slice_height = decoder->height;
        decoder->visible_width = decoder->width;
        decoder->visible_height = decoder->height;
        decoder->DefaultPlanes();

        AMediaFormat* const format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->height);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, ColorFormatYuv420Flexible);
        const media_status_t configured = AMediaCodec_configure(codec, format, nullptr, nullptr, 0);
        AMediaFormat_delete(format);
        const media_status_t started =
            configured == AMEDIA_OK ? AMediaCodec_start(codec) : AMEDIA_ERROR_UNKNOWN;
        if (configured != AMEDIA_OK || started != AMEDIA_OK) {
            LOG_ERROR(HW_GPU, "nx2 video: MediaCodec \"{}\" failed to start ({}, {})", mime,
                      static_cast<int>(configured), static_cast<int>(started));
            return nullptr;
        }
        decoder->started = true;
        char* name = nullptr;
        if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name != nullptr) {
            LOG_INFO(HW_GPU, "nx2 video: MediaCodec decoder {} for \"{}\" {}x{}", name, mime,
                     decoder->width, decoder->height);
            AMediaCodec_releaseName(codec, name);
        } else {
            LOG_INFO(HW_GPU, "nx2 video: MediaCodec decoder for \"{}\" {}x{}", mime,
                     decoder->width, decoder->height);
        }
        return decoder;
    }

    ~MediaCodecDecoder() {
        if (started) {
            AMediaCodec_stop(codec);
        }
        AMediaCodec_delete(codec);
    }

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
        // While every input slot is taken, collect finished frames to free one.
        for (int attempt = 0; attempt < 250; ++attempt) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 2000);
            if (index >= 0) {
                size_t capacity{};
                u8* const buffer =
                    AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index), &capacity);
                const size_t size = buffer != nullptr ? std::min(capacity, data.size()) : 0;
                if (size < data.size()) {
                    LOG_WARNING(HW_GPU, "nx2 video: frame of {} bytes cut to MediaCodec's {}",
                                data.size(), capacity);
                }
                if (size != 0) {
                    std::memcpy(buffer, data.data(), size);
                }
                AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, size, pts, 0);
                ++in_flight;
                last_progress = std::chrono::steady_clock::now();
                Receive(pictures, 0);
                return;
            }
            Receive(pictures, 0);
        }
        LOG_WARNING(HW_GPU, "nx2 video: MediaCodec took no input, frame dropped");
    }

    void Drain(std::vector<Nx2Picture>& pictures) {
        bool queued = false;
        for (int attempt = 0; attempt < 250 && !queued; ++attempt) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 2000);
            if (index >= 0) {
                AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                queued = true;
            } else {
                Receive(pictures, 0);
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (queued && !end_of_stream && std::chrono::steady_clock::now() < deadline) {
            Receive(pictures, 10000);
        }
        Flush();
    }

    /// Drops everything in flight; the codec takes input again right after.
    void Flush() {
        AMediaCodec_flush(codec);
        end_of_stream = false;
        in_flight = 0;
    }

    bool HasPendingOutput() const {
        return in_flight > 0;
    }

    void Poll(std::vector<Nx2Picture>& pictures) {
        Receive(pictures, 0);
        // A frame the decoder never shows (or a stream left without its end) would keep the
        // caller polling: give up on it after a second without output.
        if (in_flight > 0 &&
            std::chrono::steady_clock::now() - last_progress > std::chrono::seconds{1}) {
            in_flight = 0;
        }
    }

private:
    static constexpr s32 ColorFormatYuv420Planar = 19;
    static constexpr s32 ColorFormatYuv420PackedPlanar = 20;
    static constexpr s32 ColorFormatYuv420Flexible = 0x7F420888;

    /// Where a plane's samples are in an output buffer.
    struct Plane {
        size_t offset{};  ///< Of its first sample
        size_t col_inc{}; ///< Bytes from one sample of a row to the next
        size_t row_inc{}; ///< Bytes from one row to the next
    };

    explicit MediaCodecDecoder(AMediaCodec* codec_) : codec{codec_} {}

    /// Collects finished frames: waits up to first_timeout_us for the first, then takes only what
    /// is ready.
    void Receive(std::vector<Nx2Picture>& pictures, s64 first_timeout_us) {
        s64 timeout = first_timeout_us;
        while (true) {
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout);
            timeout = 0;
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                ReadOutputFormat();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (index < 0) {
                return;
            }
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                end_of_stream = true;
            }
            if (info.size > 0) {
                size_t capacity{};
                const u8* const buffer =
                    AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index), &capacity);
                if (buffer != nullptr && info.offset >= 0 &&
                    static_cast<size_t>(info.offset) + static_cast<size_t>(info.size) <= capacity) {
                    if (auto picture = Convert(buffer + info.offset, static_cast<size_t>(info.size),
                                               info.presentationTimeUs)) {
                        pictures.push_back(std::move(*picture));
                    }
                }
                in_flight = std::max(in_flight - 1, 0);
                last_progress = std::chrono::steady_clock::now();
            }
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
            if (end_of_stream) {
                return;
            }
        }
    }

    /// The layout of the buffers for a codec that describes it only by color format: planar
    /// (I420) for the planar formats, else semi-planar (NV12, what flexible YUV is in practice).
    void DefaultPlanes() {
        const size_t luma_size = static_cast<size_t>(stride) * static_cast<size_t>(slice_height);
        planes[0] = {0, 1, static_cast<size_t>(stride)};
        if (color_format == ColorFormatYuv420Planar ||
            color_format == ColorFormatYuv420PackedPlanar) {
            const size_t chroma_stride = static_cast<size_t>(stride / 2);
            planes[1] = {luma_size, 1, chroma_stride};
            planes[2] = {luma_size + chroma_stride * static_cast<size_t>(slice_height / 2), 1,
                         chroma_stride};
        } else {
            planes[1] = {luma_size, 2, static_cast<size_t>(stride)};
            planes[2] = {luma_size + 1, 2, static_cast<size_t>(stride)};
        }
    }

    /// The layout from the format's "image-data" (MediaImage2, frameworks/native VideoAPI.h),
    /// which says exactly where each plane is: flexible YUV may be planar or semi-planar (the
    /// platform's software decoders output I420). Only 8-bit 4:2:0 YUV is taken.
    bool ReadImageData(AMediaFormat* format) {
        void* data = nullptr;
        size_t size = 0;
        if (!AMediaFormat_getBuffer(format, "image-data", &data, &size) || data == nullptr) {
            return false;
        }
        struct MediaImage2 {
            u32 type;
            u32 num_planes;
            u32 width;
            u32 height;
            u32 bit_depth;
            u32 bit_depth_allocated;
            struct {
                u32 offset;
                s32 col_inc;
                s32 row_inc;
                u32 horiz_subsampling;
                u32 vert_subsampling;
            } plane[4];
        };
        constexpr u32 MediaImageTypeYuv = 1;
        if (size < sizeof(MediaImage2)) {
            return false;
        }
        MediaImage2 image{};
        std::memcpy(&image, data, sizeof(image));
        if (image.type != MediaImageTypeYuv || image.num_planes != 3 || image.bit_depth != 8 ||
            image.bit_depth_allocated != 8) {
            if (!warned_image) {
                warned_image = true;
                LOG_ERROR(HW_GPU,
                          "nx2 video: MediaCodec image type {}, {} planes, {}-bit ({} allocated) "
                          "is not 8-bit YUV",
                          image.type, image.num_planes, image.bit_depth,
                          image.bit_depth_allocated);
            }
            unsupported = image.bit_depth != 8 || image.bit_depth_allocated != 8;
            return false;
        }
        for (u32 i = 0; i < 3; ++i) {
            const auto& plane = image.plane[i];
            const u32 subsampling = i == 0 ? 1 : 2;
            if (plane.col_inc <= 0 || plane.row_inc <= 0 ||
                plane.horiz_subsampling != subsampling || plane.vert_subsampling != subsampling ||
                (i == 0 && plane.col_inc != 1)) {
                return false;
            }
        }
        for (u32 i = 0; i < 3; ++i) {
            planes[i] = {image.plane[i].offset, static_cast<size_t>(image.plane[i].col_inc),
                         static_cast<size_t>(image.plane[i].row_inc)};
        }
        return true;
    }

    void ReadOutputFormat() {
        AMediaFormat* const format = AMediaCodec_getOutputFormat(codec);
        if (format == nullptr) {
            return;
        }
        s32 value{};
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &value) && value > 0) {
            width = value;
        }
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &value) && value > 0) {
            height = value;
        }
        stride = AMediaFormat_getInt32(format, "stride", &value) && value > 0 ? value : width;
        slice_height =
            AMediaFormat_getInt32(format, "slice-height", &value) && value > 0 ? value : height;
        stride = std::max(stride, width);
        slice_height = std::max(slice_height, height);
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &value)) {
            color_format = value;
        }
        s32 left{}, top{}, right{}, bottom{};
        if (AMediaFormat_getRect(format, "crop", &left, &top, &right, &bottom) && left >= 0 &&
            top >= 0 && right >= left && bottom >= top) {
            crop_left = left;
            crop_top = top;
            visible_width = right - left + 1;
            visible_height = bottom - top + 1;
        } else {
            crop_left = crop_top = 0;
            visible_width = width;
            visible_height = height;
        }
        // MediaFormat.COLOR_RANGE_FULL is 1; COLOR_STANDARD_BT709 1, _BT601_PAL 2, _BT601_NTSC 4,
        // _BT2020 6.
        color_range = AMediaFormat_getInt32(format, "color-range", &value) && value == 1
                          ? AVCOL_RANGE_JPEG
                          : AVCOL_RANGE_MPEG;
        color_space = AVCOL_SPC_UNSPECIFIED;
        if (AMediaFormat_getInt32(format, "color-standard", &value)) {
            color_space = value == 1   ? AVCOL_SPC_BT709
                          : value == 2 ? AVCOL_SPC_BT470BG
                          : value == 4 ? AVCOL_SPC_SMPTE170M
                          : value == 6 ? AVCOL_SPC_BT2020_NCL
                                       : AVCOL_SPC_UNSPECIFIED;
        }
        unsupported = false;
        const bool image_data = ReadImageData(format);
        if (!image_data) {
            DefaultPlanes();
        }
        LOG_INFO(HW_GPU,
                 "nx2 video: MediaCodec output {}x{} visible {}x{}+{}+{} stride {} slice {} color "
                 "format {:#x}, planes at {}/{}/{} (columns {}/{}/{}, rows {}/{}/{}) from {}",
                 width, height, visible_width, visible_height, crop_left, crop_top, stride,
                 slice_height, color_format, planes[0].offset, planes[1].offset, planes[2].offset,
                 planes[0].col_inc, planes[1].col_inc, planes[2].col_inc, planes[0].row_inc,
                 planes[1].row_inc, planes[2].row_inc,
                 image_data ? "its image data" : "its color format");
        AMediaFormat_delete(format);
    }

    std::optional<Nx2Picture> Convert(const u8* src, size_t size, s64 pts) {
        const s32 out_width = visible_width > 0 ? visible_width : width;
        const s32 out_height = visible_height > 0 ? visible_height : height;
        const size_t chroma_width = static_cast<size_t>((out_width + 1) / 2);
        const size_t chroma_height = static_cast<size_t>((out_height + 1) / 2);
        // Where a plane's visible samples start, and one past the last byte read: decoders leave
        // the last chroma row unpadded (Exynos AV1: stride 2048, slice height 1088, a buffer 128
        // bytes short of full planes).
        const auto start = [&](const Plane& plane, s32 shift) {
            return plane.offset + static_cast<size_t>(crop_top >> shift) * plane.row_inc +
                   static_cast<size_t>(crop_left >> shift) * plane.col_inc;
        };
        const auto end = [&](const Plane& plane, s32 shift, size_t columns, size_t rows) {
            return start(plane, shift) + (rows - 1) * plane.row_inc +
                   (columns - 1) * plane.col_inc + 1;
        };
        const size_t needed = out_width <= 0 || out_height <= 0
                                  ? 0
                                  : std::max({end(planes[0], 0, static_cast<size_t>(out_width),
                                                  static_cast<size_t>(out_height)),
                                              end(planes[1], 1, chroma_width, chroma_height),
                                              end(planes[2], 1, chroma_width, chroma_height)});
        if (unsupported || out_width <= 0 || out_height <= 0 || size < needed) {
            if (!warned_layout) {
                warned_layout = true;
                LOG_ERROR(HW_GPU, "nx2 video: MediaCodec frame of {} bytes, expected {} ({}x{}){}",
                          size, needed, out_width, out_height,
                          unsupported ? ", not 8-bit" : "");
            }
            return std::nullopt;
        }

        auto frame = std::make_shared<FFmpeg::Frame>();
        AVFrame* const f = frame->GetFrame();
        f->format = AV_PIX_FMT_NV12;
        f->width = out_width;
        f->height = out_height;
        if (av_frame_get_buffer(f, 0) < 0) {
            return std::nullopt;
        }
        f->pts = pts;
        f->best_effort_timestamp = pts;
        f->color_range = color_range;
        f->colorspace = color_space;

        const u8* const luma = src + start(planes[0], 0);
        for (s32 y = 0; y < out_height; ++y) {
            std::memcpy(f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0],
                        luma + static_cast<size_t>(y) * planes[0].row_inc,
                        static_cast<size_t>(out_width));
        }
        const u8* const u = src + start(planes[1], 1);
        const u8* const v = src + start(planes[2], 1);
        const bool interleaved = planes[1].col_inc == 2 && planes[2].col_inc == 2 &&
                                 planes[2].offset == planes[1].offset + 1 &&
                                 planes[1].row_inc == planes[2].row_inc;
        for (size_t y = 0; y < chroma_height; ++y) {
            u8* const dst = f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1];
            if (interleaved) {
                std::memcpy(dst, u + y * planes[1].row_inc, chroma_width * 2);
                continue;
            }
            const u8* const u_row = u + y * planes[1].row_inc;
            const u8* const v_row = v + y * planes[2].row_inc;
            for (size_t x = 0; x < chroma_width; ++x) {
                dst[x * 2] = u_row[x * planes[1].col_inc];
                dst[x * 2 + 1] = v_row[x * planes[2].col_inc];
            }
        }
        return Nx2Picture{static_cast<u32>(out_width), static_cast<u32>(out_height),
                          static_cast<u64>(pts), std::move(frame)};
    }

    AMediaCodec* codec{};
    bool started{};
    bool end_of_stream{};
    bool warned_layout{};
    bool warned_image{};
    bool unsupported{}; ///< Not 8-bit: no frame is converted
    s32 in_flight{};    ///< Frames queued and not output yet
    std::chrono::steady_clock::time_point last_progress{};
    s32 width{};
    s32 height{};
    s32 stride{};
    s32 slice_height{};
    s32 color_format{ColorFormatYuv420Flexible};
    s32 crop_left{};
    s32 crop_top{};
    s32 visible_width{};
    s32 visible_height{};
    AVColorRange color_range{AVCOL_RANGE_MPEG};
    AVColorSpace color_space{AVCOL_SPC_UNSPECIFIED};
    std::array<Plane, 3> planes{};
};
#endif

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
#ifdef EDEN_NX2_LIBGAV1
    std::unique_ptr<libgav1::Decoder> av1; ///< Instead of FFmpeg, for AV1
    u64 av1_errors{};
#endif
#ifdef __ANDROID__
    std::unique_ptr<MediaCodecDecoder> media_codec; ///< Instead of libgav1, for AV1
#endif
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

#ifdef EDEN_NX2_LIBGAV1
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
#endif
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime, [[maybe_unused]] u32 width,
                                 [[maybe_unused]] u32 height)
    : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
#ifdef __ANDROID__
    // AV1 goes to MediaCodec (hardware on most phones), else to libgav1 below.
    if (Nx2IsAv1Mime(mime)) {
        if (!MediaCodecAllowed()) {
            LOG_INFO(HW_GPU, "nx2 video: MediaCodec off (EDEN_NX2_VIDEO_MEDIACODEC=0)");
        } else if ((impl->media_codec = MediaCodecDecoder::Create("video/av01", width, height))) {
            impl->open = true;
            return;
        } else {
            LOG_WARNING(HW_GPU, "nx2 video: no MediaCodec decoder for \"{}\", decoding in "
                                "software", mime);
        }
    }
#endif
#ifdef EDEN_NX2_LIBGAV1
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
#endif
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
#ifdef __ANDROID__
    if (impl->media_codec) {
        impl->media_codec->Decode(data, pts, pictures);
        return;
    }
#endif
#ifdef EDEN_NX2_LIBGAV1
    if (impl->av1) {
        impl->DecodeAv1(data, pts, pictures);
        return;
    }
#endif
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
#ifdef __ANDROID__
    if (impl->media_codec) {
        impl->media_codec->Drain(pictures); // and flushes
        impl->pending_pts.clear();
        return;
    }
#endif
#ifdef EDEN_NX2_LIBGAV1
    if (impl->av1) {
        Reset(); // libgav1 holds no picture back (see DecodeAv1)
        return;
    }
#endif
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

bool Nx2VideoDecoder::HasPendingOutput() const {
#ifdef __ANDROID__
    return impl->media_codec && impl->media_codec->HasPendingOutput();
#else
    return false;
#endif
}

void Nx2VideoDecoder::Poll([[maybe_unused]] std::vector<Nx2Picture>& pictures) {
#ifdef __ANDROID__
    if (impl->media_codec) {
        impl->media_codec->Poll(pictures);
    }
#endif
}

void Nx2VideoDecoder::Reset() {
#ifdef __ANDROID__
    if (impl->media_codec) {
        impl->media_codec->Flush();
        impl->pending_pts.clear();
        return;
    }
#endif
#ifdef EDEN_NX2_LIBGAV1
    if (impl->av1) {
        // Releases its frames; the next frame may start a new sequence.
        impl->av1->SignalEOS();
        impl->pending_pts.clear();
        return;
    }
#endif
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
