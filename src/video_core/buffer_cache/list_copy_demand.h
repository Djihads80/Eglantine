// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"

namespace VideoCommon {

// Predicts useful eager copies only. A miss must retain the normal version check and flush path.
class ListCopyDemand {
public:
    void NoteWrite(u64 writer) {
        current_writer = writer;
    }
    bool Request() {
        if (ShouldCopy()) {
            return false;
        }
        if (count == requested.size()) {
            saturated = true;
        } else {
            requested[count++] = current_writer;
        }
        return true;
    }
    [[nodiscard]] bool ShouldCopy() const {
        return saturated || std::find(requested.begin(), requested.begin() + count,
                                      current_writer) != requested.begin() + count;
    }

private:
    u64 current_writer{};
    std::array<u64, 4> requested{};
    size_t count{};
    bool saturated{};
};

} // namespace VideoCommon
