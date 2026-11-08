// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

namespace Common {

/// A/B launch gate: true unless the environment variable is set to "0".
inline bool EnvGate(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || std::strcmp(value, "0") != 0;
}

} // namespace Common
