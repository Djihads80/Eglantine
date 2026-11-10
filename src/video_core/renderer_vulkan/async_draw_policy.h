// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Vulkan {

/// Native Switch 2 shader emission is moved to a worker only while the worker pool has room.
constexpr bool DefersShaderEmission(bool native, size_t pending, size_t capacity) {
    return native && pending < capacity;
}

} // namespace Vulkan
