// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <map>
#include <mutex>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/nx2_list_grid.h"

namespace Tegra::Nx2ListGrid {

namespace {

constexpr std::array<const char*, static_cast<size_t>(Check::Count)> check_names{
    "kept copy", "reader", "indirect grid", "patched upload", "forced resolve"};

struct Counts {
    std::array<u64, static_cast<size_t>(Check::Count)> checks{};
    std::array<u64, static_cast<size_t>(Check::Count)> mismatches{};
};

struct VerifyState {
    std::mutex mutex;
    std::map<u64, Counts> window; ///< By macro hash, since the last summary
    std::map<u64, Counts> total;
    u32 logged{};
    std::chrono::steady_clock::time_point window_start{};
};

VerifyState& State() {
    static VerifyState state;
    return state;
}

} // Anonymous namespace

void NoteVerify(Check check, u64 hash, bool match, std::string_view detail) {
    VerifyState& state = State();
    std::scoped_lock lock{state.mutex};
    const size_t index = static_cast<size_t>(check);
    for (auto* counts : {&state.window[hash], &state.total[hash]}) {
        ++counts->checks[index];
        counts->mismatches[index] += match ? 0 : 1;
    }
    if (!match && state.logged < 16) {
        ++state.logged;
        LOG_WARNING(HW_GPU, "List HLE verify mismatch ({} of macro {:016x}): {}",
                    check_names[index], hash, detail);
    }
    const auto now = std::chrono::steady_clock::now();
    if (state.window_start == std::chrono::steady_clock::time_point{}) {
        state.window_start = now;
        return;
    }
    if (now - state.window_start < std::chrono::seconds{10}) {
        return;
    }
    state.window_start = now;
    for (const auto& [macro, counts] : state.window) {
        const Counts& total = state.total[macro];
        std::string line;
        for (size_t i = 0; i < counts.checks.size(); ++i) {
            if (total.checks[i] == 0) {
                continue;
            }
            line += fmt::format("{}{} {} checked, {} mismatches ({} / {} since boot)",
                                line.empty() ? "" : "; ", check_names[i], counts.checks[i],
                                counts.mismatches[i], total.checks[i], total.mismatches[i]);
        }
        LOG_INFO(HW_GPU, "List HLE verify in 10 s, macro {:016x}: {}", macro, line);
    }
    state.window.clear();
}

} // namespace Tegra::Nx2ListGrid
