// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include "common/logging.h"
#include "video_core/host1x/native_video.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace Tegra::Host1x {

struct NativeVideoDecoder::Impl {
    AVCodecContext* codec{};
    SwsContext* scaler{};

    ~Impl() {
        sws_freeContext(scaler);
        avcodec_free_context(&codec);
    }

    bool Receive(std::vector<NativeVideoFrame>& frames) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return false;
        }
        int result;
        while ((result = avcodec_receive_frame(codec, frame)) == 0) {
            if (frame->width <= 0 || frame->width > 3840 || frame->height <= 0 ||
                frame->height > 2160 || frames.size() >= 8) {
                av_frame_free(&frame);
                return false;
            }
            scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                                         static_cast<AVPixelFormat>(frame->format), frame->width,
                                         frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                         nullptr, nullptr);
            if (!scaler) {
                av_frame_free(&frame);
                return false;
            }
            const int colorspace = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
            const int* coefficients = sws_getCoefficients(colorspace);
            sws_setColorspaceDetails(scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG,
                                    coefficients, 1, 0, 1 << 16, 1 << 16);
            NativeVideoFrame output{static_cast<u32>(frame->width), static_cast<u32>(frame->height),
                                    frame->best_effort_timestamp,
                                    std::vector<u8>(size_t{static_cast<u32>(frame->width)} *
                                                    frame->height * 4)};
            u8* destination[] = {output.rgba.data(), nullptr, nullptr, nullptr};
            const int strides[] = {frame->width * 4, 0, 0, 0};
            if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
                          destination, strides) != frame->height) {
                av_frame_free(&frame);
                return false;
            }
            frames.push_back(std::move(output));
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            LOG_ERROR(HW_GPU, "Native video receive failed: {}", result);
            return false;
        }
        return true;
    }
};

NativeVideoDecoder::NativeVideoDecoder() : impl{std::make_unique<Impl>()} {}
NativeVideoDecoder::~NativeVideoDecoder() = default;

bool NativeVideoDecoder::ConfigureH264(std::span<const u8> config) {
    if (config.empty() || config.size() > 65536) {
        return false;
    }
    avcodec_free_context(&impl->codec);
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!decoder || !(impl->codec = avcodec_alloc_context3(decoder))) {
        return false;
    }
    impl->codec->thread_count = 1;
    impl->codec->pkt_timebase = AVRational{1, 1000000};
    impl->codec->extradata = static_cast<u8*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!impl->codec->extradata) {
        return false;
    }
    std::memcpy(impl->codec->extradata, config.data(), config.size());
    impl->codec->extradata_size = static_cast<int>(config.size());
    return avcodec_open2(impl->codec, decoder, nullptr) >= 0;
}

bool NativeVideoDecoder::Decode(std::span<const u8> bytes, s64 pts, bool end,
                                std::vector<NativeVideoFrame>& frames) {
    if (!impl->codec || bytes.size() > 16 * 1024 * 1024 || (bytes.empty() && !end)) {
        return false;
    }
    if (!bytes.empty()) {
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, static_cast<int>(bytes.size())) < 0) {
            av_packet_free(&packet);
            return false;
        }
        std::memcpy(packet->data, bytes.data(), bytes.size());
        packet->pts = pts;
        packet->dts = AV_NOPTS_VALUE;
        const int result = avcodec_send_packet(impl->codec, packet);
        av_packet_free(&packet);
        if (result < 0) {
            LOG_ERROR(HW_GPU, "Native video submit failed: {}", result);
            return false;
        }
        if (!impl->Receive(frames)) {
            return false;
        }
    }
    if (end) {
        const int result = avcodec_send_packet(impl->codec, nullptr);
        if (result < 0 && result != AVERROR_EOF) {
            return false;
        }
        return impl->Receive(frames);
    }
    return true;
}

} // namespace Tegra::Host1x
