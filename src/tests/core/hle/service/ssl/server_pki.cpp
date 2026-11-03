// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/ssl/ssl_backend.h"

namespace {

// ISRG Root X2, a public root certificate whose DER encoding is 543 bytes
constexpr std::string_view IsrgRootX2Pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

std::span<const u8> Bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

} // namespace

TEST_CASE("Server PKIs decode every PEM or DER certificate they hold", "[core][ssl]") {
    const std::string two_pem = std::string{IsrgRootX2Pem} + std::string{IsrgRootX2Pem};
    std::vector<std::vector<u8>> from_pem;
    REQUIRE(Service::SSL::DecodeServerPki(Bytes(two_pem), &from_pem).IsSuccess());
    REQUIRE(from_pem.size() == 2);
    REQUIRE(from_pem[0].size() == 543);
    REQUIRE(from_pem[0] == from_pem[1]);

    std::vector<u8> two_der = from_pem[0];
    two_der.insert(two_der.end(), from_pem[1].begin(), from_pem[1].end());
    std::vector<std::vector<u8>> from_der;
    REQUIRE(Service::SSL::DecodeServerPki(two_der, &from_der).IsSuccess());
    REQUIRE(from_der == from_pem);
}

TEST_CASE("Server PKIs without a whole certificate are refused", "[core][ssl]") {
    std::vector<std::vector<u8>> der;
    REQUIRE(Service::SSL::DecodeServerPki(Bytes(IsrgRootX2Pem), &der).IsSuccess());
    REQUIRE(der.size() == 1);
    der[0].pop_back();

    std::vector<std::vector<u8>> certificates;
    CHECK(Service::SSL::DecodeServerPki({}, &certificates).IsError());
    CHECK(Service::SSL::DecodeServerPki(der[0], &certificates).IsError());
    CHECK(Service::SSL::DecodeServerPki(
              Bytes("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"),
              &certificates)
              .IsError());
    CHECK(Service::SSL::DecodeServerPki(
              Bytes("-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n"),
              &certificates)
              .IsError());
    CHECK(certificates.empty());
}
