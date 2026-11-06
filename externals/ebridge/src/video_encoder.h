// H.264 encoder for the stream. Tries, in order: FFmpeg's NVENC, QSV and x264,
// then on Windows the Media Foundation encoders (GPU, then software), then
// FFmpeg's AMF. All of them but AMF split each frame into 4 slices.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ebridge {

struct EncodedPacket {
    std::vector<uint8_t> data;   // Annex-B, SPS/PPS repeated on keyframes
    bool key = false;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // Opens an encoder for width x height @ fps. *chosen = encoder name.
    bool Open(int width, int height, int fps, int bitrate_kbps, std::string* chosen,
              std::string* error);
    void Close();
    bool IsOpen() const;

    // Converts/scales a BGRA frame of any size and encodes it.
    bool Encode(const uint8_t* bgra, int src_width, int src_height, int stride, bool force_idr,
                std::vector<EncodedPacket>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ebridge
