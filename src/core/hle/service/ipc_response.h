// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Service {

// ResponseBuilder records the result offset after handles, alignment and domain headers.
// Searching for SFCO or assuming word 2 is unsafe for replies that contain handles.
[[nodiscard]] inline std::optional<std::uint32_t> ReadResponseResult(
    std::span<const std::uint32_t> response, std::size_t payload_offset) {
    if (payload_offset >= response.size()) {
        return std::nullopt;
    }
    return response[payload_offset];
}

} // namespace Service
