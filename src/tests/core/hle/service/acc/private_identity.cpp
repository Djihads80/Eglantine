// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/uuid.h"
#include "core/hle/service/acc/private_identity.h"

namespace {

namespace PrivateServer = Service::Account::PrivateServer;

constexpr u64 ApplicationId = 0x0400A3A01EE4C000ULL;

/// Decodes unpadded base64url, or returns nothing for anything else.
std::optional<std::string> Base64UrlDecode(std::string_view text) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    u32 bits = 0;
    int bit_count = 0;
    for (const char c : text) {
        const std::size_t value = alphabet.find(c);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        bits = (bits << 6) | static_cast<u32>(value);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            out += static_cast<char>((bits >> bit_count) & 0xFF);
        }
    }
    return out;
}

std::vector<std::string_view> SplitSegments(std::string_view token) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    for (std::size_t dot = token.find('.'); dot != std::string_view::npos;
         dot = token.find('.', start)) {
        segments.push_back(token.substr(start, dot - start));
        start = dot + 1;
    }
    segments.push_back(token.substr(start));
    return segments;
}

} // Anonymous namespace

TEST_CASE("PrivateServer: the account id is the legacy function of the profile UUID",
          "[core][acc]") {
    const Common::UUID user{std::array<u8, 16>{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                               0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f}};
    const Common::UUID other{std::array<u8, 16>{0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
                                                0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};

    // Servers key their accounts on these values, so they must never change for a profile.
    REQUIRE(PrivateServer::GetAccountId(user) == 0x191a1f1c15161310ULL);
    REQUIRE(PrivateServer::GetAccountId(other) == 0xdfa9036456309afdULL);
    REQUIRE(PrivateServer::GetAccountId(user) == (user.Hash() | 0x1000000000000000ULL));
    REQUIRE(PrivateServer::GetAccountId(Common::UUID{}) != 0);
}

TEST_CASE("PrivateServer: the id token is an unsigned JWT with private-server claims",
          "[core][acc]") {
    const std::string token =
        PrivateServer::MakeIdToken(0x191a1f1c15161310, ApplicationId, 1762128000);

    // Launchers detect support for this token by the encoded header.
    REQUIRE(token.starts_with("eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."));

    const auto segments = SplitSegments(token);
    REQUIRE(segments.size() == 3);
    REQUIRE(Base64UrlDecode(segments[0]) == R"({"alg":"none","typ":"JWT"})");
    REQUIRE(Base64UrlDecode(segments[1]) ==
            R"({"iss":"https://private-server.invalid","sub":"191a1f1c15161310",)"
            R"("aud":"0400A3A01EE4C000","iat":1762128000,"exp":1762214400})");
    REQUIRE(segments[2].empty());
    REQUIRE(token.find('=') == std::string::npos);
}

TEST_CASE("PrivateServer: the id token pads identifiers and expires after its lifetime",
          "[core][acc]") {
    const std::string token = PrivateServer::MakeIdToken(0x2a, 0x0100000000001000, 5);
    const auto segments = SplitSegments(token);
    REQUIRE(segments.size() == 3);
    REQUIRE(Base64UrlDecode(segments[1]) ==
            R"({"iss":"https://private-server.invalid","sub":"000000000000002a",)"
            R"("aud":"0100000000001000","iat":5,"exp":86405})");
    REQUIRE(PrivateServer::IdTokenLifetimeSeconds > 0);
}

TEST_CASE("PrivateServer: the id token encodes every payload length", "[core][acc]") {
    // Issue times of 1, 2 and 3 digits leave each remainder of the payload length divided by 3.
    REQUIRE(PrivateServer::MakeIdToken(0x1000000000000abc, ApplicationId, 0) ==
            "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."
            "eyJpc3MiOiJodHRwczovL3ByaXZhdGUtc2VydmVyLmludmFsaWQiLCJzdWIiOiIxMDAwMDAwMDAwMDAwYWJjIiwi"
            "YXVkIjoiMDQwMEEzQTAxRUU0QzAwMCIsImlhdCI6MCwiZXhwIjo4NjQwMH0.");
    REQUIRE(PrivateServer::MakeIdToken(0x1000000000000abc, ApplicationId, 10) ==
            "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."
            "eyJpc3MiOiJodHRwczovL3ByaXZhdGUtc2VydmVyLmludmFsaWQiLCJzdWIiOiIxMDAwMDAwMDAwMDAwYWJjIiwi"
            "YXVkIjoiMDQwMEEzQTAxRUU0QzAwMCIsImlhdCI6MTAsImV4cCI6ODY0MTB9.");
    REQUIRE(PrivateServer::MakeIdToken(0x1000000000000abc, ApplicationId, 100) ==
            "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."
            "eyJpc3MiOiJodHRwczovL3ByaXZhdGUtc2VydmVyLmludmFsaWQiLCJzdWIiOiIxMDAwMDAwMDAwMDAwYWJjIiwi"
            "YXVkIjoiMDQwMEEzQTAxRUU0QzAwMCIsImlhdCI6MTAwLCJleHAiOjg2NTAwfQ.");
}
