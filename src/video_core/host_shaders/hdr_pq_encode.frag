// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SDR layers on an HDR10 swapchain: sRGB BT.709 to PQ BT.2020, with SDR white at paper white.

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

const mat3 BT709_TO_BT2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956);

vec3 SrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 NitsToPq(vec3 nits) {
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(PQ_M1));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec3(PQ_M2));
}

void main() {
    vec4 src = texture(input_texture, posPos.xy);
    vec3 c = BT709_TO_BT2020 * SrgbToLinear(clamp(src.rgb, 0.0, 1.0));
    frag_color = vec4(NitsToPq(c * PAPER_WHITE_NITS), src.a);
}
