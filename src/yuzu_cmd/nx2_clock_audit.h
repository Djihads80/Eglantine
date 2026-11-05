// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Nx2ClockAudit {
using U64 = std::uint64_t;
using S64 = std::int64_t;

constexpr bool Enabled(std::string_view value) { return value == "1"; }

constexpr bool CanReadGuestCounter(bool powered, bool paused, bool effective_multicore) {
    // Single-core GetClockTicks reads the CPU worker's non-atomic cpu_ticks.
    // This SDL-side diagnostic must never access that path concurrently.
    return powered && !paused && effective_multicore;
}

struct Config {
    U64 frequency{};
    std::uint32_t cpu_clock{}; // Effective Settings::CpuClock enum, not an assumed MHz.
    std::uint32_t speed_limit{};
    bool sync_core_speed{};
    bool multicore{};
    bool use_speed_limit{};
    bool custom_cpu_ticks{};
    U64 cpu_ticks{};
    bool operator==(const Config&) const = default;
};

struct GuestRead {
    U64 ticks{};
    Config config;
    bool config_stable{true};
};

struct Sample {
    S64 before_ns{};
    S64 after_ns{};
    GuestRead guest;
};

enum class Status { Baseline, Valid, ConfigChanged, InvalidSample, CounterRegressed, Overlap };

constexpr std::string_view Name(Status value) {
    switch (value) {
    case Status::Baseline: return "baseline";
    case Status::Valid: return "valid";
    case Status::ConfigChanged: return "config_changed";
    case Status::InvalidSample: return "invalid_sample";
    case Status::CounterRegressed: return "counter_regressed";
    case Status::Overlap: return "overlapping_brackets";
    }
    return "unknown";
}

struct Report {
    Status status{Status::Baseline};
    Sample current;
    std::optional<Sample> previous;
    U64 delta_ticks{};
    U64 wall_min_ns{};
    U64 wall_max_ns{};
    double ratio_mid{};
    double ratio_low{};
    double ratio_high{};
};

// Only called after checking new >= old. Unsigned subtraction avoids signed
// overflow for valid intervals straddling the signed clock representation's zero.
constexpr U64 Difference(S64 newer, S64 older) {
    return static_cast<U64>(newer) - static_cast<U64>(older);
}

class Audit {
public:
    void Reset() {
        previous.reset();
        last_attempt.reset();
    }

    // The disabled path invokes NONE of the supplied functions, including the
    // host clock. No guest memory, GPU work, sleep, or PerfStats is involved.
    template <class HostNow, class ReadGuest, class Emit>
    void Poll(bool enabled, bool running, HostNow&& host_now, ReadGuest&& read_guest, Emit&& emit) {
        if (!enabled || !running) {
            Reset();
            return;
        }
        const S64 before = host_now();
        if (last_attempt && before >= *last_attempt && Difference(before, *last_attempt) < PeriodNs)
            return;
        last_attempt = before; // No catch-up bursts after a stalled SDL loop.
        Sample current{before, 0, read_guest()};
        current.after_ns = host_now();
        Report report{.current = current, .previous = previous};
        if (current.after_ns < current.before_ns || current.guest.config.frequency == 0 ||
            !current.guest.config_stable ||
            (current.guest.config.sync_core_speed && current.guest.config.speed_limit == 0)) {
            report.status = Status::InvalidSample;
            previous.reset();
        } else if (!previous) {
            previous = current;
        } else if (current.guest.config != previous->guest.config) {
            report.status = Status::ConfigChanged;
            previous = current;
        } else if (current.guest.ticks < previous->guest.ticks) {
            report.status = Status::CounterRegressed;
            previous = current;
        } else if (current.before_ns <= previous->after_ns) {
            report.status = Status::Overlap;
            previous = current;
        } else {
            report.status = Status::Valid;
            report.delta_ticks = current.guest.ticks - previous->guest.ticks;
            report.wall_min_ns = Difference(current.before_ns, previous->after_ns);
            report.wall_max_ns = Difference(current.after_ns, previous->before_ns);
            const long double guest_ns = static_cast<long double>(report.delta_ticks) * 1'000'000'000.0L /
                                         current.guest.config.frequency;
            const long double wall_mid = (static_cast<long double>(report.wall_min_ns) + report.wall_max_ns) / 2;
            report.ratio_mid = static_cast<double>(guest_ns / wall_mid);
            report.ratio_low = static_cast<double>(guest_ns / report.wall_max_ns);
            report.ratio_high = static_cast<double>(guest_ns / report.wall_min_ns);
            previous = current;
        }
        emit(report);
    }

private:
    static constexpr U64 PeriodNs = 1'000'000'000;
    std::optional<Sample> previous;
    std::optional<S64> last_attempt;
};
} // namespace Nx2ClockAudit
