// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace Tegra::Host1x {

class H264PacketConverter {
public:
    void Convert(std::span<u8> data) {
        if (format == Format::AnnexB) {
            return;
        }
        const bool length_prefixed = IsLengthPrefixed(data);
        if (format == Format::Unknown) {
            const bool annex_b = HasStartCode(data);
            // An ambiguous packet cannot establish the framing of the stream.
            if (length_prefixed == annex_b) {
                return;
            }
            format = length_prefixed ? Format::LengthPrefixed : Format::AnnexB;
        }
        if (format != Format::LengthPrefixed || !length_prefixed) {
            return;
        }

        std::size_t offset = 0;
        while (offset < data.size()) {
            const u32 length = ReadLength(data.subspan(offset));
            data[offset] = 0;
            data[offset + 1] = 0;
            data[offset + 2] = 0;
            data[offset + 3] = 1;
            offset += 4 + length;
        }
    }

private:
    enum class Format { Unknown, AnnexB, LengthPrefixed };

    static u32 ReadLength(std::span<const u8> data) {
        return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16) |
               (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
    }

    static bool IsLengthPrefixed(std::span<const u8> data) {
        if (data.empty()) {
            return false;
        }
        while (data.size() >= 4) {
            const u32 length = ReadLength(data);
            data = data.subspan(4);
            if (length == 0 || length > data.size()) {
                return false;
            }
            data = data.subspan(length);
        }
        return data.empty();
    }

    static bool HasStartCode(std::span<const u8> data) {
        return (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
               (data.size() >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
    }

    Format format{Format::Unknown};
};

} // namespace Tegra::Host1x
