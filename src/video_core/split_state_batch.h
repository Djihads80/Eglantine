// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdlib>

#include "common/common_types.h"

namespace VideoCommon::SplitStateBatch {

constexpr u32 Capacity = 64;
constexpr u32 FirstEngineMethod = 0x40;
constexpr u32 FirstMacroMethod = 0xe00;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_STATE_BATCH");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

struct Write {
    u32 method;
    u32 argument;
    bool is_last;
};

// Only the current Increasing packet, never a puller/macro/control execution boundary.
template <size_t N>
u32 IncreasingPrefix(u32 method, u32 pending, size_t available, bool non_incrementing,
                     bool increment_once, const std::bitset<N>& execution_mask) {
    if (non_incrementing || increment_once || method < FirstEngineMethod ||
        method >= FirstMacroMethod) {
        return 0;
    }
    const size_t limit = std::min({size_t{pending}, available, size_t{Capacity}});
    u32 count = 0;
    while (count < limit && method + count < FirstMacroMethod && method + count < N &&
           !execution_mask[method + count]) {
        ++count;
    }
    return count;
}

struct ShadowValue {
    u32 value;
    bool kept;
};

inline ShadowValue ApplyShadow(u32& shadow, u32 argument, bool tracking, bool replay) {
    if (tracking) {
        const bool kept = shadow == argument;
        shadow = argument;
        return {argument, kept};
    }
    return {replay ? shadow : argument, true};
}

inline bool ApplyState(u32& reg, ShadowValue shadow, bool skip_same) {
    if (reg == shadow.value && shadow.kept && skip_same) {
        return false;
    }
    reg = shadow.value;
    return true;
}

} // namespace VideoCommon::SplitStateBatch
