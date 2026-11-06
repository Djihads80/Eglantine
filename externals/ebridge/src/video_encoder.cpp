#include "video_encoder.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#ifdef EBRIDGE_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif
#ifdef _WIN32
#include "nv12.h"
#include "video_encoder_mf.h"
#endif

namespace ebridge {

namespace {

#ifdef EBRIDGE_HAVE_FFMPEG
struct Candidate {
    const char* name;
    // Low-latency private options; unknown ones are ignored.
    std::vector<std::pair<const char*, const char*>> options;
};

// Order matters: the Switch decodes in software, and only spreads a frame over
// its cores when the frame is split into slices. NVENC, QSV and x264 honour
// ctx->slices; AMF always writes one slice per frame (checked on an RX 7900 XTX),
// which leaves the Switch on one core and too slow for real games. So on an AMD
// PC x264 on the CPU is used, and AMF only as a last resort.
const std::vector<Candidate>& Candidates() {
    static const std::vector<Candidate> list = {
        {"h264_nvenc",
         {{"preset", "p1"}, {"tune", "ull"}, {"zerolatency", "1"}, {"delay", "0"},
          {"rc", "cbr"}, {"forced-idr", "1"}}},
        {"h264_qsv",
         {{"preset", "veryfast"}, {"async_depth", "1"}, {"forced_idr", "1"}}},
        {"libx264",
         {{"preset", "ultrafast"}, {"tune", "zerolatency"}, {"forced-idr", "1"},
          {"x264-params", "slices=4:sliced-threads=1:threads=4"}}},
        {"h264_amf",
         {{"usage", "ultralowlatency"}, {"quality", "speed"}, {"rc", "cbr"}, {"latency", "1"},
          {"async_depth", "1"}, {"forced_idr", "1"}, {"header_spacing", "1"}}},
    };
    return list;
}

std::string AvError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}
#endif

} // namespace

struct VideoEncoder::Impl {
    int width = 0, height = 0;
#ifdef EBRIDGE_HAVE_FFMPEG
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int64_t pts = 0;

    bool OpenFfmpeg(const Candidate& c, int fps, int bitrate_kbps, std::string* errors);
    bool EncodeFfmpeg(const uint8_t* bgra, int src_width, int src_height, int stride, bool force_idr,
                      std::vector<EncodedPacket>& out);
#endif
#ifdef _WIN32
    std::unique_ptr<MfEncoder> mf;
    std::vector<uint8_t> nv12;
#endif

    bool IsOpen() const {
#ifdef EBRIDGE_HAVE_FFMPEG
        if (ctx) return true;
#endif
#ifdef _WIN32
        if (mf) return true;
#endif
        return false;
    }

    void Free() {
#ifdef EBRIDGE_HAVE_FFMPEG
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        pts = 0;
#endif
#ifdef _WIN32
        mf.reset();
#endif
    }
};

#ifdef EBRIDGE_HAVE_FFMPEG
bool VideoEncoder::Impl::OpenFfmpeg(const Candidate& c, int fps, int bitrate_kbps,
                                    std::string* errors) {
    const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
    if (!codec) return false;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    context->width = width;
    context->height = height;
    context->time_base = {1, fps};
    context->framerate = {fps, 1};
    context->pix_fmt = AV_PIX_FMT_NV12;
    context->bit_rate = int64_t{bitrate_kbps} * 1000;
    context->rc_max_rate = context->bit_rate;
    context->rc_buffer_size = static_cast<int>(context->bit_rate / fps);  // ~1 frame: low latency
    context->gop_size = fps * 5;          // safety keyframe every 5 s, plus on request
    context->max_b_frames = 0;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->slices = 4;                  // lets the Switch decode with 4 cores in parallel
    context->colorspace = AVCOL_SPC_BT709;
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_BT709;
    context->color_range = AVCOL_RANGE_MPEG;
    for (const auto& [k, v] : c.options) av_opt_set(context->priv_data, k, v, 0);

    const int rc = avcodec_open2(context, codec, nullptr);
    if (rc < 0) {
        *errors += std::string(c.name) + ": " + AvError(rc) + "; ";
        avcodec_free_context(&context);
        return false;
    }
    ctx = context;
    frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_NV12;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    packet = av_packet_alloc();
    return true;
}

bool VideoEncoder::Impl::EncodeFfmpeg(const uint8_t* bgra, int src_width, int src_height, int stride,
                                      bool force_idr, std::vector<EncodedPacket>& out) {
    sws = sws_getCachedContext(sws, src_width, src_height, AV_PIX_FMT_BGRA, width, height,
                               AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    // Full-range RGB in, limited-range BT.709 out (what the Switch renderer expects).
    sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

    if (av_frame_make_writable(frame) < 0) return false;
    const uint8_t* src[1] = {bgra};
    const int src_stride[1] = {stride};
    sws_scale(sws, src, src_stride, 0, src_height, frame->data, frame->linesize);

    frame->pts = pts++;
    frame->pict_type = force_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100)
    if (force_idr) frame->flags |= AV_FRAME_FLAG_KEY;
    else frame->flags &= ~AV_FRAME_FLAG_KEY;
#else
    frame->key_frame = force_idr ? 1 : 0;
#endif

    if (avcodec_send_frame(ctx, frame) < 0) return false;
    for (;;) {
        const int rc = avcodec_receive_packet(ctx, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return false;
        EncodedPacket p;
        p.data.assign(packet->data, packet->data + packet->size);
        p.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(p));
        av_packet_unref(packet);
    }
    return true;
}
#endif

VideoEncoder::VideoEncoder() : impl(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() { Close(); }

bool VideoEncoder::IsOpen() const { return impl->IsOpen(); }

void VideoEncoder::Close() { impl->Free(); }

bool VideoEncoder::Open(int width, int height, int fps, int bitrate_kbps, std::string* chosen,
                        std::string* error) {
    Close();
    auto& d = *impl;
    d.width = width;
    d.height = height;
    std::string errors;
    // EBRIDGE_ENCODER=<name> keeps only that encoder: libx264, h264_nvenc, mf-gpu, mf-software...
    const char* only = std::getenv("EBRIDGE_ENCODER");
    const auto wanted = [only](const char* name) {
        return !only || !only[0] || std::strcmp(only, name) == 0;
    };

    // 1. FFmpeg's encoders that write slices (NVENC, QSV, x264), when this FFmpeg has them.
#ifdef EBRIDGE_HAVE_FFMPEG
    const auto& candidates = Candidates();
    for (size_t i = 0; i + 1 < candidates.size(); i++) {
        if (wanted(candidates[i].name) && d.OpenFfmpeg(candidates[i], fps, bitrate_kbps, &errors)) {
            if (chosen) *chosen = candidates[i].name;
            return true;
        }
    }
#endif
    // 2. Windows' own encoders: the GPU one if it writes slices, else the software one.
#ifdef _WIN32
    for (const bool hardware : {true, false}) {
        if (!wanted(hardware ? "mf-gpu" : "mf-software")) continue;
        auto mf = std::make_unique<MfEncoder>();
        std::string name, err;
        if (mf->Open(width, height, fps, bitrate_kbps, hardware, &name, &err)) {
            d.mf = std::move(mf);
            d.nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
            if (chosen) *chosen = name;
            return true;
        }
        errors += std::string(hardware ? "MF GPU: " : "MF software: ") + err + " ";
    }
#endif
    // 3. AMF, one slice per frame: slow on the Switch, but better than no picture.
#ifdef EBRIDGE_HAVE_FFMPEG
    if (wanted(candidates.back().name) && d.OpenFfmpeg(candidates.back(), fps, bitrate_kbps, &errors)) {
        if (chosen) *chosen = candidates.back().name;
        return true;
    }
#endif
    if (error) *error = errors.empty() ? "no H.264 encoder available" : errors;
    return false;
}

bool VideoEncoder::Encode(const uint8_t* bgra, int src_width, int src_height, int stride,
                          bool force_idr, std::vector<EncodedPacket>& out) {
    auto& d = *impl;
#ifdef EBRIDGE_HAVE_FFMPEG
    if (d.ctx) return d.EncodeFfmpeg(bgra, src_width, src_height, stride, force_idr, out);
#endif
#ifdef _WIN32
    if (d.mf) {
        uint8_t* y = d.nv12.data();
        uint8_t* uv = y + static_cast<size_t>(d.width) * d.height;
        BgraToNv12(bgra, src_width, src_height, stride, y, d.width, uv, d.width, d.width, d.height);
        return d.mf->Encode(d.nv12.data(), force_idr, out);
    }
#endif
    (void)bgra;
    (void)src_width;
    (void)src_height;
    (void)stride;
    (void)force_idr;
    (void)out;
    return false;
}

} // namespace ebridge
