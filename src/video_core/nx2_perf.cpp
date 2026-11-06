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

constexpr u64 all_opts = ~u64{0} >> (64 - static_cast<unsigned>(Opt::Count));

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

std::string Names(u64 mask) {
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
void Poll(std::stop_token stop, std::string path_text, u64 startup_mask, bool enabled) {
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
                        ParseControl(startup_mask, content, enabled ? live_safe : 0);
                    const u64 effective = enabled ? result.mask : all_opts;
                    detail::live_off_mask.store(effective, std::memory_order_relaxed);
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

void StartPoller(const char* path, u64 startup_mask, bool enabled) {
    try {
        static std::jthread poller{Poll, std::string{path}, startup_mask, enabled};
    } catch (const std::system_error& error) {
        LOG_ERROR(HW_GPU, "Nx2PerfLive: no poller thread: {}", error.what());
    }
}

} // Anonymous namespace

ControlResult ParseControl(u64 startup_mask, std::string_view text, u64 live_safe_mask) {
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
                    result.mask &= ~(u64{1} << *index);
                } else {
                    result.mask |= u64{1} << *index;
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

std::string FormatControl(const ControlResult& result, u64 effective_mask) {
    return fmt::format("set={} on={} off={} ignored={} mask={:#018x}",
                       result.label.empty() ? "-" : result.label, Names(result.on_vs_startup),
                       Names(result.off_vs_startup), result.ignored.empty() ? "-" : result.ignored,
                       effective_mask);
}

namespace detail {

u64 InitOffMask() {
    [[maybe_unused]] static const bool initialized = [] {
        const char* on_list = std::getenv("EDEN_NX2_PERF_ON");
        const char* off_list = std::getenv("EDEN_NX2_PERF_OFF");
        const char* frames_ahead = std::getenv("EDEN_NX2_PERF_FRAMES_AHEAD");
        const u64 mask = ParseOffMask(std::getenv("EDEN_NX2_PERF_SAFE") != nullptr,
                                      on_list ? on_list : "", off_list ? off_list : "",
                                      frames_ahead && frames_ahead[0] == '0');
        std::string state;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            state += fmt::format(" {}={}", opt_names[i], Enabled() && !IsOff(mask, i) ? "on" : "off");
        }
        LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
        // Stored before the poller starts, so its first store is never overwritten.
        live_off_mask.store(Enabled() ? mask : all_opts, std::memory_order_relaxed);
        if (const char* path = std::getenv("EDEN_NX2_PERF_CONTROL"); path && path[0] != '\0') {
            StartPoller(path, mask, Enabled());
        }
        return true;
    }();
    // The live value: with every switch off (all 64 bits) it equals the sentinel and On() comes
    // back here each time, which still answers right.
    return live_off_mask.load(std::memory_order_relaxed);
}

} // namespace detail

} // namespace VideoCommon::Nx2Perf
