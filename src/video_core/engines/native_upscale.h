// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace Tegra::Engines {

struct NativeUpscaleRegions {
    // Half-open coordinates; allocation dimensions stay unchanged for tiled addressing.
    std::array<u32, 4> source;
    std::array<u32, 4> destination;
};

// The compact DLSS parameter layout has a dispatch header, followed by inclusive
// source/destination rectangles at 0x10/0x20. Match the header before using this layout.
inline std::optional<NativeUpscaleRegions> DecodeNativeUpscaleRegions(
    std::span<const u32> words, const std::array<u32, 4>& dispatch,
    const std::array<u32, 2>& source_size, const std::array<u32, 2>& destination_size) {
    if (words.size() < 12 || !std::equal(dispatch.begin(), dispatch.end(), words.begin())) {
        return std::nullopt;
    }
    const auto rectangle = [](const u32* data, const std::array<u32, 2>& size)
        -> std::optional<std::array<u32, 4>> {
        if (data[0] > data[2] || data[1] > data[3] || data[2] >= size[0] || data[3] >= size[1]) {
            return std::nullopt;
        }
        return std::array<u32, 4>{data[0], data[1], data[2] + 1, data[3] + 1};
    };
    const auto source = rectangle(words.data() + 4, source_size);
    const auto destination = rectangle(words.data() + 8, destination_size);
    if (!source || !destination || (*destination)[2] - (*destination)[0] < (*source)[2] - (*source)[0] ||
        (*destination)[3] - (*destination)[1] < (*source)[3] - (*source)[1]) {
        return std::nullopt;
    }
    return NativeUpscaleRegions{*source, *destination};
}

} // namespace Tegra::Engines
