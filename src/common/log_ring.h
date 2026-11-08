// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/common_types.h"

// The in-memory log ring (the debug server's log tail, docs/nx2/debug-mcp.md). Kept out of
// logging.h, which nearly every file includes.
namespace Common::Log {

/// A line kept by the in-memory log ring.
struct RingLine {
    u64 sequence;
    std::string text;
};

/// Keeps the last `capacity` formatted lines in memory; 0 turns the ring off and empties it.
void SetRingCapacity(std::size_t capacity);

/// The kept lines numbered above `after`, oldest first.
[[nodiscard]] std::vector<RingLine> ReadRing(u64 after);

} // namespace Common::Log
