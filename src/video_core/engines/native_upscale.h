// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"

namespace Tegra::Engines {

template <typename Image, typename ReadImage, typename IsUpscalePair>
std::optional<u32> FindNativeUpscaleOutput(const std::optional<Image>& input,
                                         std::span<const u32> stores, ReadImage&& image,
                                         IsUpscalePair&& upscale_pair) {
    if (!input) return std::nullopt;
    for (const u32 store : stores) {
        if (upscale_pair(input, image(store), false)) return store;
    }
    return std::nullopt;
}

template <typename ReadImage, typename IsUpscalePair>
std::optional<std::pair<u32, u32>> FindNativeUpscaleSlots(
    std::span<const u32> reads, std::span<const u32> stores, ReadImage&& image,
    IsUpscalePair&& upscale_pair) {
    for (const u32 store : stores) {
        const auto to = image(store);
        for (const bool same_format : {true, false}) {
            std::optional<std::pair<u32, u32>> selected;
            u64 area{};
            for (const u32 read : reads) {
                const auto from = image(read);
                if (upscale_pair(from, to, same_format) &&
                    (!selected || u64{from->Width()} * from->Height() > area)) {
                    selected = std::pair{read, store};
                    area = u64{from->Width()} * from->Height();
                }
            }
            if (selected) {
                return selected;
            }
        }
    }
    return std::nullopt;
}

} // namespace Tegra::Engines
