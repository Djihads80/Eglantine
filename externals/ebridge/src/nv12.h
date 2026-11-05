// BGRA (full range) to NV12 (BT.709, limited range), scaled to the output size.
// Used by the encoders that don't go through FFmpeg's libswscale.
#pragma once

#include <cstdint>

namespace ebridge {

// dst_y: width x height bytes, dst_uv: width x height / 2 bytes (interleaved U, V).
// width and height must be even.
void BgraToNv12(const uint8_t* bgra, int src_width, int src_height, int src_stride, uint8_t* dst_y,
                int y_stride, uint8_t* dst_uv, int uv_stride, int width, int height);

} // namespace ebridge
