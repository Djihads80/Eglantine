// SPDX-License-Identifier: GPL-3.0-or-later

#version 450

layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

// The verified native reconstruction applies x*abs(x) after sampling its sqrt-encoded input.
// Keep the resize's sampling order and alpha. This is not the piecewise sRGB transfer function.
void main() {
    const vec4 value = textureLod(tex, texcoord, 0);
    color = vec4(value.rgb * abs(value.rgb), value.a);
}
