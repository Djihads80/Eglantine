// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Stands in for a game's DLSS evaluation (the tensor-core network cannot run here): a temporal
// upscale. Each frame's input samples are placed where the camera jitter put them and weighted by
// their distance to the output pixel; they accumulate with the previous output, reprojected along
// the game's motion vectors (bicubic) and clipped to the current 3x3 neighbourhood.

#version 450

layout(binding = 0) uniform sampler2D input_color;
layout(binding = 1) uniform sampler2D motion_vectors;
layout(binding = 2) uniform sampler2D history;

layout(push_constant) uniform PushConstants {
    // The vertex stage's texture scale and offset come first.
    layout(offset = 16) vec2 jitter;  // this frame's sample offset, in input pixels
    vec2 motion_scale;                // motion vector -> UV offset to the previous frame
    float current_weight;             // largest weight of this frame; 1 ignores the history
};

layout(location = 0) in vec2 texcoord;  // 0 to 1 across the output
layout(location = 0) out vec4 color;

// The blit helper's samplers take unnormalized (texel) coordinates.

// Blending in a range-compressed space keeps bright pixels from dominating the average.
vec3 Compress(vec3 c) {
    return c / (1.0 + max(max(c.r, c.g), c.b));
}

vec3 Expand(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(1.0 - 1.0 / 65504.0));
    return c / (1.0 - max(max(c.r, c.g), c.b));
}

vec3 RgbToYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)),
                dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 YCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Moves value towards the box centre until it lies inside the box.
vec3 ClipToBox(vec3 value, vec3 low, vec3 high) {
    const vec3 center = 0.5 * (low + high);
    const vec3 extent = 0.5 * (high - low) + 1.0 / 65504.0;
    const vec3 offset = value - center;
    const vec3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : value;
}

// Catmull-Rom filtered history from five bilinear fetches: repeated bilinear reprojection would
// blur the accumulated image a little more every frame.
vec3 SampleHistory(vec2 position) {
    const vec2 texel1 = floor(position - 0.5) + 0.5;
    const vec2 f = position - texel1;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 texel0 = texel1 - 1.0;
    const vec2 texel3 = texel1 + 2.0;
    const vec2 texel12 = texel1 + w2 / w12;
    vec3 sum = texture(history, vec2(texel12.x, texel0.y)).rgb * (w12.x * w0.y);
    sum += texture(history, vec2(texel0.x, texel12.y)).rgb * (w0.x * w12.y);
    sum += texture(history, texel12).rgb * (w12.x * w12.y);
    sum += texture(history, vec2(texel3.x, texel12.y)).rgb * (w3.x * w12.y);
    sum += texture(history, vec2(texel12.x, texel3.y)).rgb * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / weight, vec3(0.0));
}

void main() {
    const ivec2 input_size = textureSize(input_color, 0);
    const ivec2 motion_size = textureSize(motion_vectors, 0);
    // The output pixel's centre in input pixels. Input texel k sampled the scene at k + 0.5 - jitter
    // (the sign measured against consecutive frames).
    const vec2 target = texcoord * vec2(input_size);
    const ivec2 nearest = ivec2(floor(target + jitter));

    vec3 filtered = vec3(0.0);
    float total_weight = 0.0;
    float nearest_weight = 0.0;
    vec3 sum = vec3(0.0);
    vec3 squares = vec3(0.0);
    vec2 motion = vec2(0.0);
    float longest = -1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 texel = clamp(nearest + ivec2(x, y), ivec2(0), input_size - 1);
            const vec3 sample_color = RgbToYCoCg(Compress(texelFetch(input_color, texel, 0).rgb));
            // A Blackman-Harris-like window over the sample's distance to the output pixel.
            const vec2 distance = vec2(texel) + 0.5 - jitter - target;
            const float weight = exp(-2.29 * dot(distance, distance));
            filtered += sample_color * weight;
            total_weight += weight;
            nearest_weight = max(nearest_weight, weight);
            sum += sample_color;
            squares += sample_color * sample_color;
            // The longest motion around the pixel, so that an edge follows the object in front.
            const vec2 candidate =
                texelFetch(motion_vectors, clamp(texel, ivec2(0), motion_size - 1), 0).rg;
            const float length_squared = dot(candidate, candidate);
            if (length_squared > longest) {
                longest = length_squared;
                motion = candidate;
            }
        }
    }
    const vec3 current = filtered / total_weight;
    const vec3 mean = sum / 9.0;
    const vec3 deviation = sqrt(max(squares / 9.0 - mean * mean, vec3(0.0)));
    const vec3 low = mean - deviation;
    const vec3 high = mean + deviation;

    const vec2 previous = texcoord + motion * motion_scale;
    // A frame whose samples land close to this pixel counts more; one whose samples land between
    // pixels mostly keeps the history (from other jitter positions) there.
    float weight = current_weight * nearest_weight;
    if (current_weight >= 1.0 || any(lessThan(previous, vec2(0.0))) ||
        any(greaterThan(previous, vec2(1.0)))) {
        weight = 1.0;
    }
    const vec2 history_position = previous * vec2(textureSize(history, 0));
    const vec3 past = ClipToBox(RgbToYCoCg(Compress(SampleHistory(history_position))), low, high);
    color = vec4(Expand(YCoCgToRgb(mix(past, current, weight))), 1.0);
}
