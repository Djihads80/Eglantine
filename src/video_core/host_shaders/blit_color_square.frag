// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450

layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

// Copy of a source that stores the square root of each color channel: squares them back,
// alpha unchanged.
void main() {
    const vec4 value = textureLod(tex, texcoord, 0);
    color = vec4(value.rgb * value.rgb, value.a);
}
