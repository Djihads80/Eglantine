// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::NativeUpscale {

// This verified program reads sqrt-encoded RGB9E5. Its reconstruction contains 32 HMUL2
// x*abs(x) instructions, covering RGB for 16 output pixels, before the final sharpening/stores.
// A resize fallback omits the network and sharpening, but must retain that output conversion.
// This is deliberately an exact program allowlist, not a guess from image brightness or format.
inline constexpr size_t SignedSquareProgramBytes = 20496;
inline constexpr u64 SignedSquareProgramKey = 0x6f9367bd41dc0b58ULL;

constexpr u64 ProgramKey(std::span<const u8> bytes) {
    u64 hash = 14695981039346656037ULL;
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

// Diagnostic only: never broaden the global tensor mode or select an adjacent program.
constexpr bool TensorGateEnabled(std::string_view value, bool global_tensor_present) {
    return value == "1" && !global_tensor_present;
}

constexpr bool SelectTensorReconstruction(bool enabled, bool compute, u64 program_key,
                                          size_t program_bytes) {
    return enabled && compute && program_bytes == SignedSquareProgramBytes &&
           program_key == SignedSquareProgramKey;
}

constexpr bool UsesSignedSquare(u64 program_key, size_t program_bytes,
                                Tegra::Texture::TextureFormat input,
                                Tegra::Texture::TextureFormat output) {
    return program_bytes == SignedSquareProgramBytes && program_key == SignedSquareProgramKey &&
           input == Tegra::Texture::TextureFormat::E5B9G9R9 &&
           output == Tegra::Texture::TextureFormat::B10G11R11;
}

} // namespace VideoCommon::NativeUpscale
