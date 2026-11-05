// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdio>
#include <latch>
#include <string_view>
#include <thread>
#include <windows.h>

#include "common/cpu_features.h"
#include "common/scm_rev.h"
#include "common/thread.h"
#include "common/windows/timer_resolution.h"

namespace {

long long Qpc() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string_view{argv[1]} != "--maximum-resolution")) {
        std::fputs("Usage: event_wait_probe [--maximum-resolution]\n", stderr);
        return 2;
    }
    if (argc == 2) {
        Common::Windows::SetCurrentTimerResolutionToMaximum();
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const auto ns = [&](long long ticks) { return ticks * 1'000'000'000LL / frequency.QuadPart; };
    std::printf("commit=%s qpc_hz=%lld tsc_hz=%llu monitorx=%d waitpkg=%d timer_resolution_ns=%lld\n",
                Common::g_scm_rev, frequency.QuadPart,
                static_cast<unsigned long long>(Common::g_cpu_caps.tsc_frequency),
                Common::g_cpu_caps.monitorx, Common::g_cpu_caps.waitpkg,
                static_cast<long long>(Common::Windows::GetCurrentTimerResolution().count()));
    std::puts("trial,requested_ns,requested_signal_delay_ns,wait_elapsed_ns,signal_call_ns,signal_done_ns,result,is_set_at_return,is_set_after_join");
    unsigned failures{};
    for (int trial = 0; trial < 30; ++trial) {
        Common::Event event;
        std::latch ready{1};
        long long signal_begin{}, signal_end{};
        std::jthread setter([&] {
            ready.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            signal_begin = Qpc();
            event.Set();
            signal_end = Qpc();
        });
        ready.count_down();
        const auto begin = Qpc();
        const bool result = event.WaitFor(std::chrono::seconds{2});
        const auto end = Qpc();
        const bool state = event.IsSet();
        setter.join();
        std::printf("%d,2000000000,5000000,%lld,%lld,%lld,%d,%d,%d\n", trial,
                    ns(end - begin), ns(signal_begin - begin), ns(signal_end - begin),
                    result, state, event.IsSet());
        failures += !result || state || event.IsSet() || end < signal_begin;
    }
    for (const long long timeout : {200000LL, 500000LL, 1000000LL, 10000000LL,
                                    1000000000LL, 2000000000LL}) {
        for (int trial = 0; trial < 5; ++trial) {
            Common::Event event;
            const auto begin = Qpc();
            const bool result = event.WaitFor(std::chrono::nanoseconds{timeout});
            const auto elapsed = ns(Qpc() - begin);
            std::printf("%d,%lld,-1,%lld,-1,-1,%d,%d,%d\n", trial, timeout, elapsed,
                        result, event.IsSet(), event.IsSet());
            failures += result || event.IsSet() || elapsed < timeout;
        }
    }
    std::fprintf(stderr, "Event wait probe: %u failures\n", failures);
    return failures == 0 ? 0 : 1;
}
