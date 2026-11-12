// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

#include "video_core/nx2_timeline.h"

namespace VideoCommon::Nx2Timeline {

u64 ThreadCpuNow() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto ticks = [](const FILETIME& time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    // 100 ns units.
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
#endif
}

} // namespace VideoCommon::Nx2Timeline
