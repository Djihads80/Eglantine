// H.264 through Windows Media Foundation: the GPU encoders (NVIDIA, AMD, Intel)
// or Microsoft's software encoder, all part of Windows. Used when the FFmpeg the
// host is built with has no H.264 encoder (e.g. a decode-only bundled FFmpeg).
#pragma once
#ifdef _WIN32

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ebridge {

struct EncodedPacket;

class MfEncoder {
public:
    MfEncoder();
    ~MfEncoder();

    // hardware: a GPU encoder (rejected if it can't split frames into slices),
    // otherwise Microsoft's software encoder. *name describes what was opened.
    bool Open(int width, int height, int fps, int bitrate_kbps, bool hardware, std::string* name,
              std::string* error);

    // nv12: width x height luma followed by the interleaved chroma plane.
    bool Encode(const uint8_t* nv12, bool force_idr, std::vector<EncodedPacket>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ebridge

#endif
