#include "nv12.h"

#include <algorithm>
#include <vector>

namespace ebridge {

namespace {

// BT.709 limited range, 8-bit fixed point (x256).
inline uint8_t LumaOf(int r, int g, int b) {
    return static_cast<uint8_t>(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8));
}
inline uint8_t CbOf(int r, int g, int b) {
    return static_cast<uint8_t>(128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8));
}
inline uint8_t CrOf(int r, int g, int b) {
    return static_cast<uint8_t>(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8));
}

} // namespace

void BgraToNv12(const uint8_t* bgra, int src_width, int src_height, int src_stride, uint8_t* dst_y,
                int y_stride, uint8_t* dst_uv, int uv_stride, int width, int height) {
    if (src_width == width && src_height == height) {
        // Same size (the emulator's renderer already gives 1280x720): no scaling.
        for (int y = 0; y < height; y += 2) {
            const uint8_t* r0 = bgra + static_cast<size_t>(y) * src_stride;
            const uint8_t* r1 = r0 + src_stride;
            uint8_t* y0 = dst_y + static_cast<size_t>(y) * y_stride;
            uint8_t* y1 = y0 + y_stride;
            uint8_t* uv = dst_uv + static_cast<size_t>(y / 2) * uv_stride;
            for (int x = 0; x < width; x += 2) {
                const uint8_t* a = r0 + x * 4;
                const uint8_t* b = r1 + x * 4;
                y0[x] = LumaOf(a[2], a[1], a[0]);
                y0[x + 1] = LumaOf(a[6], a[5], a[4]);
                y1[x] = LumaOf(b[2], b[1], b[0]);
                y1[x + 1] = LumaOf(b[6], b[5], b[4]);
                const int red = (a[2] + a[6] + b[2] + b[6] + 2) >> 2;
                const int green = (a[1] + a[5] + b[1] + b[5] + 2) >> 2;
                const int blue = (a[0] + a[4] + b[0] + b[4] + 2) >> 2;
                uv[x] = CbOf(red, green, blue);
                uv[x + 1] = CrOf(red, green, blue);
            }
        }
        return;
    }
    // Source column of each output column, and the weight of the next one (0..255):
    // bilinear when scaling, an exact copy when the sizes match.
    std::vector<int> sx(width), wx(width);
    for (int x = 0; x < width; x++) {
        const int64_t pos = (static_cast<int64_t>(2 * x + 1) * src_width * 256) / (2 * width) - 128;
        const int64_t p = std::clamp<int64_t>(pos, 0, int64_t{src_width - 1} * 256);
        sx[x] = static_cast<int>(p >> 8);
        wx[x] = sx[x] + 1 < src_width ? static_cast<int>(p & 255) : 0;
    }
    std::vector<uint8_t> rgb_rows(static_cast<size_t>(width) * 3 * 2);

    for (int y = 0; y < height; y += 2) {
        for (int k = 0; k < 2; k++) {
            const int oy = y + k;
            const int64_t pos =
                (static_cast<int64_t>(2 * oy + 1) * src_height * 256) / (2 * height) - 128;
            const int64_t p = std::clamp<int64_t>(pos, 0, int64_t{src_height - 1} * 256);
            const int row = static_cast<int>(p >> 8);
            const int wy = row + 1 < src_height ? static_cast<int>(p & 255) : 0;
            const uint8_t* r0 = bgra + static_cast<size_t>(row) * src_stride;
            const uint8_t* r1 = wy ? r0 + src_stride : r0;
            uint8_t* rgb = rgb_rows.data() + static_cast<size_t>(k) * width * 3;
            uint8_t* out_y = dst_y + static_cast<size_t>(oy) * y_stride;
            for (int x = 0; x < width; x++) {
                const uint8_t* a = r0 + sx[x] * 4;
                const uint8_t* c = r1 + sx[x] * 4;
                const int fx = wx[x];
                const uint8_t* b = fx ? a + 4 : a;
                const uint8_t* d = fx ? c + 4 : c;
                int ch[3];
                for (int i = 0; i < 3; i++) {
                    const int top = a[i] * (256 - fx) + b[i] * fx;
                    const int bottom = c[i] * (256 - fx) + d[i] * fx;
                    ch[i] = (top * (256 - wy) + bottom * wy + (1 << 15)) >> 16;
                }
                const int blue = ch[0], green = ch[1], red = ch[2];
                rgb[x * 3 + 0] = static_cast<uint8_t>(red);
                rgb[x * 3 + 1] = static_cast<uint8_t>(green);
                rgb[x * 3 + 2] = static_cast<uint8_t>(blue);
                out_y[x] = LumaOf(red, green, blue);
            }
        }
        // Chroma: average of each 2x2 block.
        const uint8_t* t = rgb_rows.data();
        const uint8_t* u = t + static_cast<size_t>(width) * 3;
        uint8_t* out_uv = dst_uv + static_cast<size_t>(y / 2) * uv_stride;
        for (int x = 0; x < width; x += 2) {
            const int r = (t[x * 3] + t[x * 3 + 3] + u[x * 3] + u[x * 3 + 3] + 2) >> 2;
            const int g = (t[x * 3 + 1] + t[x * 3 + 4] + u[x * 3 + 1] + u[x * 3 + 4] + 2) >> 2;
            const int b = (t[x * 3 + 2] + t[x * 3 + 5] + u[x * 3 + 2] + u[x * 3 + 5] + 2) >> 2;
            out_uv[x] = CbOf(r, g, b);
            out_uv[x + 1] = CrOf(r, g, b);
        }
    }
}

} // namespace ebridge
