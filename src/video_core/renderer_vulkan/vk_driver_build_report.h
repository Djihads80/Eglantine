// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace Vulkan::DriverBuildReport {

enum class Kind { Graphics, Compute };

/// Records one vkCreate*Pipelines call. Every 10 s with builds, logs how many there were and how
/// many took over 10 ms: a driver that finds the pipeline in its cache builds it in about 1 ms, one
/// that compiles it takes tens of ms (NVIDIA: 0.6-1.5 ms against ~48 ms), so tester logs show
/// whether the driver's own cache works on their system. Thread safe.
void Record(Kind kind, std::chrono::steady_clock::duration time);

/// Builds recorded on this thread while it lives are the shader precache's: they are counted
/// apart, so the game's own builds show whether the driver's cache served them.
class PrecacheScope {
public:
    PrecacheScope();
    ~PrecacheScope();
    PrecacheScope(const PrecacheScope&) = delete;
    PrecacheScope& operator=(const PrecacheScope&) = delete;
};

/// Logs the builds since the last summary (the pipelines built at boot) and starts over.
void LogSummary(const char* when);

} // namespace Vulkan::DriverBuildReport
