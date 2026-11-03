// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/core.h"
#include "mcp/context.h"

namespace Mcp {

bool ToolContext::GameRunning() const {
    return system != nullptr && system->IsPoweredOn() && !system->IsShuttingDown();
}

} // namespace Mcp
