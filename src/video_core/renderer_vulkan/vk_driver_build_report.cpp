// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_spikes.h"
#include "video_core/renderer_vulkan/vk_driver_build_report.h"

namespace Vulkan::DriverBuildReport {
namespace {

// Over this, the driver compiled the pipeline instead of taking it from its cache.
constexpr std::chrono::milliseconds CompiledThreshold{10};

thread_local bool precache_thread{};

struct Bucket {
    u64 count{};
    u64 compiled{};
    std::chrono::steady_clock::duration total{};
    std::chrono::steady_clock::duration max{};

    void Add(std::chrono::steady_clock::duration time) {
        ++count;
        compiled += time > CompiledThreshold ? 1 : 0;
        total += time;
        max = std::max(max, time);
    }

    std::string Format(const char* what) const {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        if (count == 0) {
            return fmt::format("0 {}", what);
        }
        return fmt::format("{} {}, {} over {} ms, {:.1f} ms mean, {:.1f} ms max", count, what,
                           compiled, CompiledThreshold.count(),
                           ms(total) / static_cast<double>(count), ms(max));
    }
};

struct Totals {
    Bucket graphics;
    Bucket compute;
    Bucket precache;

    void Add(Kind kind, std::chrono::steady_clock::duration time) {
        (precache_thread ? precache : kind == Kind::Graphics ? graphics : compute).Add(time);
    }

    void Log(const char* what) const {
        // Over the threshold: compiled, not from the driver's cache.
        LOG_INFO(Render_Vulkan, "Driver pipeline builds {}: {}; {}{}", what,
                 graphics.Format("graphics"), compute.Format("compute"),
                 precache.count != 0 ? "; shader precache: " + precache.Format("compute")
                                     : std::string{});
    }
};

std::mutex mutex;
Totals window;
Totals summary;
std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};

} // Anonymous namespace

PrecacheScope::PrecacheScope() {
    precache_thread = true;
}

PrecacheScope::~PrecacheScope() {
    precache_thread = false;
}

void Record(Kind kind, std::chrono::steady_clock::duration time) {
    if (!precache_thread) {
        using VideoCommon::Nx2Spikes::Event;
        VideoCommon::Nx2Spikes::Note(Event::DriverBuild, time);
        if (time > CompiledThreshold) {
            VideoCommon::Nx2Spikes::Note(Event::DriverCompile, time);
        }
    }
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
