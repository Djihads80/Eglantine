// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "video_core/nx2_perf.h"

namespace VideoCommon::Nx2Perf {
namespace {

constexpr u64 all_opts = ~u64{0} >> (64 - static_cast<unsigned>(Opt::Count));

} // Anonymous namespace

namespace detail {

u64 InitOffMask() {
    [[maybe_unused]] static const bool initialized = [] {
        const char* on_list = std::getenv("EDEN_NX2_PERF_ON");
        const char* off_list = std::getenv("EDEN_NX2_PERF_OFF");
        const u64 mask = ParseOffMask(std::getenv("EDEN_NX2_PERF_SAFE") != nullptr,
                                      on_list ? on_list : "", off_list ? off_list : "");
        std::string state;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            state += fmt::format(" {}={}", opt_names[i], Enabled() && !IsOff(mask, i) ? "on" : "off");
        }
        LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
        live_off_mask.store(Enabled() ? mask : all_opts, std::memory_order_relaxed);
        return true;
    }();
    // With every switch off (all 64 bits) the mask equals the sentinel and On() comes back here
    // each time, which still answers right.
    return live_off_mask.load(std::memory_order_relaxed);
}

} // namespace detail

} // namespace VideoCommon::Nx2Perf
