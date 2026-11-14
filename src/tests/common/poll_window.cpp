// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "common/thread.h"

using namespace std::chrono_literals;
using Common::NextPollWindow;

TEST_CASE("Poll windows keep what they catch", "[common][thread]") {
    CHECK(NextPollWindow(100us, 3us, 100us) == 100us);
    CHECK(NextPollWindow(25us, 25us, 100us) == 25us);
}

TEST_CASE("Poll windows grow for waits a longer poll would catch", "[common][thread]") {
    CHECK(NextPollWindow(25us, 40us, 100us) == 50us);
    CHECK(NextPollWindow(50us, 100us, 100us) == 100us);
    CHECK(NextPollWindow(75us, 90us, 100us) == 100us);
}

TEST_CASE("Poll windows shrink for waits no poll catches, down to an eighth", "[common][thread]") {
    CHECK(NextPollWindow(100us, 101us, 100us) == 50us);
    CHECK(NextPollWindow(50us, 2ms, 100us) == 25us);
    CHECK(NextPollWindow(25us, 2ms, 100us) == 12500ns);
    CHECK(NextPollWindow(12500ns, 2ms, 100us) == 12500ns);
}
