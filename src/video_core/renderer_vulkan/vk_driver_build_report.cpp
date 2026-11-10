// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_driver_build_report.h"

namespace Vulkan::DriverBuildReport {
namespace {

// Over this, the driver compiled the pipeline instead of taking it from its cache.
constexpr std::chrono::milliseconds CompiledThreshold{10};

struct Totals {
    u64 graphics{};
    u64 compute{};
    u64 compiled{};
    std::chrono::steady_clock::duration total{};
    std::chrono::steady_clock::duration max{};

    void Add(Kind kind, std::chrono::steady_clock::duration time) {
        ++(kind == Kind::Graphics ? graphics : compute);
        compiled += time > CompiledThreshold ? 1 : 0;
        total += time;
        max = std::max(max, time);
    }

    void Log(const char* what) const {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        const u64 count = graphics + compute;
        LOG_INFO(Render_Vulkan,
                 "Driver pipeline builds {}: {} graphics, {} compute; {} over {} ms (compiled, not "
                 "from the driver's cache); {:.1f} ms mean, {:.1f} ms max",
                 what, graphics, compute, compiled, CompiledThreshold.count(),
                 count != 0 ? ms(total) / static_cast<double>(count) : 0.0, ms(max));
    }
};

std::mutex mutex;
Totals window;
Totals summary;
std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};

} // Anonymous namespace

void Record(Kind kind, std::chrono::steady_clock::duration time) {
    std::scoped_lock lock{mutex};
    window.Add(kind, time);
    summary.Add(kind, time);
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start >= std::chrono::seconds{10}) {
        window.Log("in the last 10 s");
        window = {};
        window_start = now;
    }
}

void LogSummary(const char* when) {
    std::scoped_lock lock{mutex};
    summary.Log(when);
    summary = {};
}

} // namespace Vulkan::DriverBuildReport
