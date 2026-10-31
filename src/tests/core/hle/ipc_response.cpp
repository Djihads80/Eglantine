// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/ipc_response.h"

TEST_CASE("IPC diagnostics read the result after response metadata", "[core][hle][ipc]") {
    // CMIF with no handles: SFCO, version, result, padding.
    const std::array<std::uint32_t, 8> cmif{0, 10, 0, 0, 0x4f434653, 0, 0xca01, 0};
    CHECK(Service::ReadResponseResult(cmif, 6) == 0xca01);

    // CMIF with handles and a domain header: the real SFCO is beyond word 11.
    // A handle can itself equal SFCO; it must not be mistaken for the payload header.
    const std::array<std::uint32_t, 16> domain{
        0x80000000, 14, 0x40, 0x4f434653, 0, 0, 0, 0,
        1, 0, 0, 0, 0x4f434653, 0, 0xca01, 0};
    CHECK(Service::ReadResponseResult(domain, 14) == 0xca01);

    // TIPC has no SFCO; copy/move handles move the result away from word 2.
    const std::array<std::uint32_t, 3> tipc{16, 1, 0xca01};
    const std::array<std::uint32_t, 5> tipc_handles{0x80000010, 1, 0x20, 0x1234, 0xca01};
    CHECK(Service::ReadResponseResult(tipc, 2) == 0xca01);
    CHECK(Service::ReadResponseResult(tipc_handles, 4) == 0xca01);
}

TEST_CASE("IPC diagnostics distinguish success from a missing reply", "[core][hle][ipc]") {
    const std::array<std::uint32_t, 3> success{16, 1, 0};
    const auto result = Service::ReadResponseResult(success, 2);
    REQUIRE(result.has_value());
    CHECK(*result == 0);
    CHECK_FALSE(Service::ReadResponseResult({}, 0).has_value());
    CHECK_FALSE(Service::ReadResponseResult(success, success.size()).has_value());
    CHECK_FALSE(Service::ReadResponseResult(success, std::numeric_limits<std::size_t>::max())
                    .has_value());
}
