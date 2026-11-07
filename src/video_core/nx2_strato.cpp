// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_quick_bind.h"
#include "video_core/nx2_speculate.h"
#include "video_core/nx2_strato.h"

namespace VideoCommon::Nx2Strato {

namespace {

constexpr u64 WindowNs = 5'000'000'000ULL;

// CPU time of the calling thread, in nanoseconds.
u64 ThreadCpuNs() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto ticks = [](const FILETIME& time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return static_cast<u64>(time.tv_sec) * 1'000'000'000ULL + static_cast<u64>(time.tv_nsec);
#endif
}

std::atomic<u64> g_window_start{};
// The drawing thread's CPU time at its last sample (NoteConfigure), and which thread that is.
std::atomic<u64> g_draw_cpu_ns{};
std::atomic<size_t> g_draw_thread{};
std::atomic<u64> g_reported_cpu_ns{}; // at the last report (0: no baseline)

constexpr std::array<const char*, static_cast<size_t>(Wait::Count)> wait_names{
    "list copy", "list flush", "DMA flush", "split writers"};

} // Anonymous namespace

u64 Now() {
    return Nx2Profile::Now();
}

void NoteConfigure(u64 ns) {
    g_configure_ns.fetch_add(ns, std::memory_order_relaxed);
    const u64 calls = g_configure_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls & 255) == 0) {
        const size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        if (g_draw_thread.exchange(thread, std::memory_order_relaxed) != thread) {
            // Another thread draws now: no delta this window.
            g_reported_cpu_ns.store(0, std::memory_order_relaxed);
        }
        g_draw_cpu_ns.store(ThreadCpuNs(), std::memory_order_relaxed);
    }
    MaybeReport();
}

void MaybeReport() {
    if (!Tegra::Nx2StratoStats()) {
        return;
    }
    const u64 now = Now();
    u64 start = g_window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        g_window_start.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < WindowNs ||
        !g_window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        return;
    }
    const double seconds = static_cast<double>(now - start) / 1e9;
    const auto per_second = [seconds](u64 value) { return static_cast<double>(value) / seconds; };
    const auto ms_per_second = [seconds](u64 ns) { return static_cast<double>(ns) / 1e6 / seconds; };
    std::string line;
    for (size_t i = 0; i < g_waits.size(); ++i) {
        WaitTotals& totals = g_waits[i];
        const u64 count = totals.count.exchange(0, std::memory_order_relaxed);
        const u64 ns = totals.ns.exchange(0, std::memory_order_relaxed);
        const u64 done_count = totals.done_count.exchange(0, std::memory_order_relaxed);
        const u64 done_ns = totals.done_ns.exchange(0, std::memory_order_relaxed);
        const u64 gen_skips = totals.gen_skips.exchange(0, std::memory_order_relaxed);
        line += fmt::format(" | {} {:.0f} waits {:.1f} ms (write done: {:.0f} {:.1f} ms; real: "
                            "{:.0f} {:.1f} ms), gen skipped {:.0f}",
                            wait_names[i], per_second(count), ms_per_second(ns),
                            per_second(done_count), ms_per_second(done_ns),
                            per_second(count - done_count), ms_per_second(ns - done_ns),
                            per_second(gen_skips));
        if (static_cast<Wait>(i) == Wait::ListCopy) {
            const u64 same = g_same_words_count.exchange(0, std::memory_order_relaxed);
            const u64 same_ns = g_same_words_ns.exchange(0, std::memory_order_relaxed);
            line += fmt::format(" (same words as the read before: {:.0f} {:.1f} ms)",
                                per_second(same), ms_per_second(same_ns));
        }
    }
    const u64 gen_downloads = g_gen_downloads.exchange(0, std::memory_order_relaxed);
    const u64 gen_downloads_free = g_gen_downloads_free.exchange(0, std::memory_order_relaxed);
    if (gen_downloads != 0) {
        line += fmt::format(" | gen downloads {:.0f} waited for an older tick ({:.0f} not at all)",
                            per_second(gen_downloads), per_second(gen_downloads_free));
    }
    const u64 configure_ns = g_configure_ns.exchange(0, std::memory_order_relaxed);
    const u64 configure_calls = g_configure_calls.exchange(0, std::memory_order_relaxed);
    const u64 cpu_ns = g_draw_cpu_ns.load(std::memory_order_relaxed);
    const u64 reported_cpu_ns = g_reported_cpu_ns.exchange(cpu_ns, std::memory_order_relaxed);
    const u64 cpu_delta =
        reported_cpu_ns != 0 && cpu_ns > reported_cpu_ns ? cpu_ns - reported_cpu_ns : 0;
    line += fmt::format(" | configure {:.0f} draws {:.1f} ms ({:.2f} us each) of the drawing "
                        "thread's {:.1f} ms CPU ({:.0f}%)",
                        per_second(configure_calls), ms_per_second(configure_ns),
                        configure_calls != 0 ? static_cast<double>(configure_ns) / 1e3 /
                                                   static_cast<double>(configure_calls)
                                             : 0.0,
                        ms_per_second(cpu_delta),
                        cpu_delta != 0 ? 100.0 * static_cast<double>(configure_ns) /
                                             static_cast<double>(cpu_delta)
                                       : 0.0);
    LOG_INFO(HW_GPU, "Strato stats per second over {:.1f} s (GPU-written page waits){}", seconds,
             line);
}

} // namespace VideoCommon::Nx2Strato

namespace Tegra {

void Nx2ReportQuickBind() {
    Nx2QuickBindStats& stats = g_nx2_quick_bind;
    const u64 now = VideoCommon::Nx2Strato::Now();
    u64 start = stats.window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 10'000'000'000ULL ||
        !stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        return;
    }
    const u64 draws = stats.draws.exchange(0, std::memory_order_relaxed);
    const u64 hits = stats.hits.exchange(0, std::memory_order_relaxed);
    const u64 mismatches = stats.mismatches.exchange(0, std::memory_order_relaxed);
    const u64 stale = stats.stale.exchange(0, std::memory_order_relaxed);
    LOG_INFO(HW_GPU,
             "Quick binds in {:.1f} s: {} draws, {} reused their textures ({:.0f}%){}",
             static_cast<double>(now - start) / 1e9, draws, hits,
             draws != 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(draws) : 0.0,
             Nx2QuickBind() == 2 ? fmt::format("; verify: {} DIFFERED, {} had an image to prepare",
                                               mismatches, stale)
                                 : std::string{});
}

void Nx2ReportSpeculation() {
    Nx2SpeculationStats& stats = g_nx2_speculation;
    const u64 now = VideoCommon::Nx2Strato::Now();
    u64 start = stats.window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 10'000'000'000ULL ||
        !stats.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        return;
    }
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const u64 reads = take(stats.reads);
    const u64 waits = take(stats.waits);
    const u64 same = take(stats.same);
    const u64 predicted = take(stats.predicted);
    const u64 predicted_waits = take(stats.predicted_waits);
    const u64 right = take(stats.right);
    const u64 wrong = take(stats.wrong);
    const u64 waited_ns = take(stats.waited_ns);
    const u64 global = take(stats.global);
    const u64 sign_ns = take(g_nx2_sign_ns);
    LOG_INFO(HW_GPU,
             "Command list speculation in {:.1f} s: {} copy reads with a read before ({} "
             "waited), {} got the same words; {} written by a dispatch with global accesses{}; "
             "predicted the same for {} ({} of them waited): {} right, {} "
             "WRONG; {:.1f} ms of waits a right prediction would have saved, {:.1f} ms signing",
             static_cast<double>(now - start) / 1e9, reads, waits, same, global,
             Nx2ClSpeculateStrict() ? " (not predicted)" : "", predicted,
             predicted_waits, right, wrong, static_cast<double>(waited_ns) / 1e6,
             static_cast<double>(sign_ns) / 1e6);
}

} // namespace Tegra
