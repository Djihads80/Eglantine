// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <span>
#include <vector>
#include "common/common_types.h"

namespace Tegra::Host1x {

struct NativeVideoFrame {
    u32 width{};
    u32 height{};
    s64 pts{};
    std::vector<u8> rgba;
};

class NativeVideoDecoder {
public:
    NativeVideoDecoder();
    ~NativeVideoDecoder();
    bool ConfigureH264(std::span<const u8> config);
    bool Decode(std::span<const u8> packet, s64 pts, bool end,
                std::vector<NativeVideoFrame>& frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Tegra::Host1x
