// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Games that see an HDR display draw RGB10A2 in BT.2020 with the PQ curve.
// Bring that back to sRGB for an SDR swapchain.

#version 460

#ifdef VULKAN

#define BINDING_COLOR_TEXTURE 1

#else // ^^^ Vulkan ^^^ // vvv OpenGL vvv

#define BINDING_COLOR_TEXTURE 0

#endif

layout (location = 0) in vec4 posPos;

layout (location = 0) out vec4 frag_color;

layout (binding = BINDING_COLOR_TEXTURE) uniform sampler2D input_texture;

const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;

const float PAPER_WHITE_NITS = 203.0;
const float PEAK_NITS = 1000.0;

const mat3 BT2020_TO_BT709 = mat3(
    1.6605, -0.1246, -0.0182,
   -0.5876,  1.1329, -0.1006,
   -0.0728, -0.0083,  1.1187);

vec3 PqToNits(vec3 e) {
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));
    return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), vec3(1.0 / PQ_M1));
}

vec3 LinearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec4 src = texture(input_texture, posPos.xy);
    vec3 c = max(BT2020_TO_BT709 * (PqToNits(src.rgb) / PAPER_WHITE_NITS), 0.0);

    // Extended Reinhard on the brightest channel, so hue holds as highlights roll off
    float m = max(max(c.r, c.g), c.b);
    if (m > 0.0) {
        const float w = PEAK_NITS / PAPER_WHITE_NITS;
        c *= (1.0 + m / (w * w)) / (1.0 + m);
    }
    frag_color = vec4(LinearToSrgb(clamp(c, 0.0, 1.0)), src.a);
}
