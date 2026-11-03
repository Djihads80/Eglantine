// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// RCAS (FidelityFX FSR 1's contrast-adaptive sharpening) on the DLSS stand-in's result, before the
// game post-processes it. The input is linear HDR; RCAS's limits assume 0 to 1, so it sharpens in
// the stand-in's range-compressed space.

#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(binding = 0) uniform sampler2D InputTexture;

layout(push_constant) uniform PushConstants {
    // The vertex stage's texture scale and offset come first.
    layout(offset = 16) uvec4 Const0; // FsrRcasCon
};

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

vec3 Compress(vec3 c) {
    return c / (1.0 + max(max(c.r, c.g), c.b));
}

vec3 Expand(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(1.0 - 1.0 / 65504.0));
    return c / (1.0 - max(max(c.r, c.g), c.b));
}

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    return AF4(Compress(texelFetch(InputTexture, p, 0).rgb), 1.0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void main() {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), Const0);
    color = vec4(Expand(c), 1.0);
}
