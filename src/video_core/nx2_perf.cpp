// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "common/thread.h"
#include "video_core/nx2_perf.h"

namespace VideoCommon::Nx2Perf {
namespace {

constexpr OffMask all_opts = [] {
    OffMask mask{};
    for (size_t i = 0; i < static_cast<size_t>(Opt::Count); ++i) {
        mask |= Bit(static_cast<Opt>(i));
    }
    return mask;
}();

void StoreLive(const OffMask& mask) {
    detail::live_off_mask[1].store(mask.hi, std::memory_order_relaxed);
    detail::live_off_mask[0].store(mask.lo, std::memory_order_relaxed);
}

std::string_view Trim(std::string_view text) {
    constexpr std::string_view space = " \t\r\v\f";
    const size_t begin = text.find_first_not_of(space);
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(space) - begin + 1);
}

void Append(std::string& list, std::string_view name) {
    if (!list.empty()) {
        list += ',';
    }
    list += name;
}

std::string Names(const OffMask& mask) {
    std::string list;
    for (size_t i = 0; i < opt_names.size(); ++i) {
        if (IsOff(mask, i)) {
            Append(list, opt_names[i]);
        }
    }
    return list.empty() ? std::string{"-"} : list;
}

std::optional<size_t> Find(std::string_view name) {
    for (size_t i = 0; i < opt_names.size(); ++i) {
        if (opt_names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

/// The live poller: never calls On() (it runs while InitOffMask's static may still be
/// initializing).
void Poll(std::stop_token stop, std::string path_text, OffMask startup_mask, bool enabled) {
    Common::SetCurrentThreadName("Nx2PerfLive");
    // Logged by the thread itself: the lab takes this line as proof that flips get applied.
    LOG_INFO(HW_GPU, "Nx2PerfLive: watching {} live_safe={}", path_text,
             enabled ? Names(live_safe) : std::string{"-"});
    const std::filesystem::path path{path_text};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>> stamp;
    std::string applied_text;
    bool applied = false;
    while (!stop.stop_requested()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        const auto time = ec ? std::filesystem::file_time_type{}
                             : std::filesystem::last_write_time(path, ec);
        if (!ec && stamp != std::make_pair(size, time)) {
            // The writer replaces the file (temp + rename): a failed read keeps the previous
            // state and is retried on the next poll.
            std::ifstream file(path, std::ios::binary);
            std::string content;
            if (file) {
                content.assign(std::istreambuf_iterator<char>{file}, {});
            }
            // A size other than the stat's: replaced in between, read again on the next poll.
            if (file && !file.bad() && content.size() == size) {
                stamp = std::make_pair(size, time);
                if (!applied || content != applied_text) {
                    // EDEN_NX2_PERF_OPT=0 keeps every switch off: nothing is live-safe then.
                    const ControlResult result =
                        ParseControl(startup_mask, content, enabled ? live_safe : OffMask{});
                    const OffMask effective = enabled ? result.mask : all_opts;
                    StoreLive(effective);
                    LOG_INFO(HW_GPU, "Nx2PerfLive: {}", FormatControl(result, effective));
                    applied_text = std::move(content);
                    applied = true;
                }
            }
        }
        std::unique_lock lock{mutex};
        wake.wait_for(lock, stop, std::chrono::milliseconds{100}, [] { return false; });
    }
}

void StartPoller(const char* path, const OffMask& startup_mask, bool enabled) {
    try {
        static std::jthread poller{Poll, std::string{path}, startup_mask, enabled};
    } catch (const std::system_error& error) {
        LOG_ERROR(HW_GPU, "Nx2PerfLive: no poller thread: {}", error.what());
    }
}

} // Anonymous namespace

ControlResult ParseControl(const OffMask& startup_mask, std::string_view text,
                           const OffMask& live_safe_mask) {
    ControlResult result{.mask = startup_mask};
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const size_t space = line.find_first_of(" \t");
        const std::string_view directive = line.substr(0, space);
        std::string_view rest =
            space == std::string_view::npos ? std::string_view{} : Trim(line.substr(space));
        if (directive == "label") {
            result.label = std::string{rest.substr(0, rest.find_first_of(" \t"))};
        } else if (directive == "safe" && rest.empty()) {
            result.mask |= live_safe_mask & (DefaultOffMask() | always_off);
        } else if (directive == "on" || directive == "off") {
            while (!rest.empty()) {
                const size_t end = rest.find_first_of(", \t");
                const std::string_view name = rest.substr(0, end);
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
                if (name.empty()) {
                    continue;
                }
                const std::optional<size_t> index = Find(name);
                if (!index || !IsOff(live_safe_mask, *index)) {
                    Append(result.ignored, name);
                } else if (directive == "on") {
                    result.mask &= ~Bit(static_cast<Opt>(*index));
                } else {
                    result.mask |= Bit(static_cast<Opt>(*index));
                }
            }
        } else {
            Append(result.ignored, directive);
        }
    }
    result.on_vs_startup = startup_mask & ~result.mask;
    result.off_vs_startup = result.mask & ~startup_mask;
    return result;
}

std::string FormatControl(const ControlResult& result, const OffMask& effective_mask) {
    return fmt::format("set={} on={} off={} ignored={} mask=0x{:016x}{:016x}",
                       result.label.empty() ? "-" : result.label, Names(result.on_vs_startup),
                       Names(result.off_vs_startup), result.ignored.empty() ? "-" : result.ignored,
                       effective_mask.hi, effective_mask.lo);
}

namespace detail {

OffMask InitOffMask() {
    [[maybe_unused]] static const bool initialized = [] {
        const char* on_list = std::getenv("EDEN_NX2_PERF_ON");
        const char* off_list = std::getenv("EDEN_NX2_PERF_OFF");
        const char* frames_ahead = std::getenv("EDEN_NX2_PERF_FRAMES_AHEAD");
        const OffMask mask = ParseOffMask(std::getenv("EDEN_NX2_PERF_SAFE") != nullptr,
                                      on_list ? on_list : "", off_list ? off_list : "",
                                      frames_ahead && frames_ahead[0] == '0');
        std::string state;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            state += fmt::format(" {}={}", opt_names[i], Enabled() && !IsOff(mask, i) ? "on" : "off");
        }
        LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
        // Stored before the poller starts, so its first store is never overwritten.
        StoreLive(Enabled() ? mask : all_opts);
        if (const char* path = std::getenv("EDEN_NX2_PERF_CONTROL"); path && path[0] != '\0') {
            StartPoller(path, mask, Enabled());
        }
        return true;
    }();
    // The live value: a word with every switch off equals the sentinel and On() comes back here
    // each time, which still answers right.
    return {live_off_mask[0].load(std::memory_order_relaxed),
            live_off_mask[1].load(std::memory_order_relaxed)};
}

} // namespace detail

namespace {

/// Once a second has passed since the last report, the nanoseconds since it for the one caller
/// that claims the report; otherwise 0. The first call only starts the clock.
s64 ClaimReport(std::atomic<s64>& last) {
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 previous = last.load(std::memory_order_relaxed);
    if (previous == 0) {
        last.compare_exchange_strong(previous, now, std::memory_order_relaxed);
        return 0;
    }
    if (now - previous < 1'000'000'000) {
        return 0;
    }
    return last.compare_exchange_strong(previous, now, std::memory_order_relaxed) ? now - previous
                                                                                  : 0;
}

constexpr std::array<std::string_view, static_cast<size_t>(GuestSite::Count)> guest_site_names{
    "QueryCounter", "RO page", "slot event", "vi2 wait"};

} // Anonymous namespace

void NoteLandedForcedSubmit() {
    static std::atomic<u64> submits{};
    static std::atomic<u64> total{};
    static std::atomic<s64> last{};
    submits.fetch_add(1, std::memory_order_relaxed);
    if (const s64 elapsed = ClaimReport(last); elapsed != 0) {
        const u64 count = submits.exchange(0, std::memory_order_relaxed);
        const u64 all = total.fetch_add(count, std::memory_order_relaxed) + count;
        LOG_INFO(HW_GPU, "Nx2Perf guest_landed_syncpoints: {:.0f} landed forced submits/s ({} in all)",
                 static_cast<double>(count) * 1e9 / static_cast<double>(elapsed), all);
    }
}

void NoteGuestObservation(GuestSite site, u32 id, u32 value, u32 host) {
    if (id >= 192 ||
        ((detail::fence_syncpoints[id / 64].load(std::memory_order_relaxed) >> (id % 64)) & 1) ==
            0) {
        return;
    }
    constexpr size_t num_sites = static_cast<size_t>(GuestSite::Count);
    static std::array<std::atomic<u64>, num_sites> checked{};
    static std::array<std::atomic<u64>, num_sites> early{};
    static std::atomic<u64> early_total{};
    static std::atomic<s64> last{};
    const size_t index = static_cast<size_t>(site);
    checked[index].fetch_add(1, std::memory_order_relaxed);
    if (static_cast<s32>(value - host) > 0) {
        early[index].fetch_add(1, std::memory_order_relaxed);
        if (early_total.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf guest_landed_syncpoints: guest saw syncpoint {}={} before it "
                      "landed (host {}, {})",
                      id, value, host, guest_site_names[index]);
        }
    }
    if (const s64 elapsed = ClaimReport(last); elapsed != 0) {
        std::string line;
        u64 early_sum = 0;
        for (size_t i = 0; i < num_sites; ++i) {
            const u64 early_count = early[i].exchange(0, std::memory_order_relaxed);
            early_sum += early_count;
            line += fmt::format(" {} {}/{}", guest_site_names[i], early_count,
                                checked[i].exchange(0, std::memory_order_relaxed));
        }
        const std::string text =
            fmt::format("Nx2Perf guest_landed_syncpoints: guest observations before landing / "
                        "checked in the last {} ms:{} ({} early in all)",
                        elapsed / 1'000'000, line, early_total.load(std::memory_order_relaxed));
        if (early_sum != 0) {
            LOG_ERROR(HW_GPU, "{}", text);
        } else {
            LOG_INFO(HW_GPU, "{}", text);
        }
    }
}

} // namespace VideoCommon::Nx2Perf
