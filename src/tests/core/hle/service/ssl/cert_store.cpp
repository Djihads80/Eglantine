// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/ssl/cert_store.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "tests/core/hle/service/ssl/private_server_ca.h"

// The certificates the ssl services' GetCertificates returns, as titles building their own trust
// store see them, with and without a private server CA.

namespace {

namespace ssl = Service::SSL;
using ssl::CaCertificateId;
using ssl::CertStore;
using ssl::TrustedCertStatus;

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

std::vector<u8> IsrgRootX2Der() {
    std::vector<std::vector<u8>> der;
    REQUIRE(ssl::DecodeServerPki(Bytes(IsrgRootX2Pem), &der).IsSuccess());
    REQUIRE(der.size() == 1);
    return der.front();
}

struct Entry {
    CaCertificateId id;
    TrustedCertStatus status;
    std::vector<u8> der;

    bool operator==(const Entry&) const = default;
};

/// An ssl_TrustedCerts.bdf holding entries, as the system's CertStore archive does
FileSys::VirtualFile MakeBuiltInCertificates(const std::vector<Entry>& entries) {
    const ssl::CertStoreHeader header{
        .magic = Common::MakeMagic('s', 's', 'l', 'T'),
        .num_entries = static_cast<u32>(entries.size()),
    };
    std::vector<u8> data(sizeof(header) + entries.size() * sizeof(ssl::CertStoreEntry));
    std::memcpy(data.data(), &header, sizeof(header));
    for (size_t i = 0; i < entries.size(); i++) {
        // Offsets count from the end of the header
        const ssl::CertStoreEntry entry{
            .certificate_id = entries[i].id,
            .certificate_status = entries[i].status,
            .der_size = static_cast<u32>(entries[i].der.size()),
            .der_offset = static_cast<u32>(data.size() - sizeof(header)),
        };
        std::memcpy(data.data() + sizeof(header) + i * sizeof(entry), &entry, sizeof(entry));
        data.insert(data.end(), entries[i].der.begin(), entries[i].der.end());
    }
    return std::make_shared<FileSys::VectorVfsFile>(std::move(data), "ssl_TrustedCerts.bdf");
}

/// What GetCertificates returns for ids, in a buffer of the size GetCertificateBufSize asks for
std::vector<Entry> GetCertificates(CertStore& store, std::vector<CaCertificateId> ids) {
    u32 size = 0;
    u32 size_entries = 0;
    REQUIRE(store.GetCertificateBufSize(&size, &size_entries, ids).IsSuccess());
    std::vector<u8> buffer(size);
    u32 num_entries = 0;
    REQUIRE(store.GetCertificates(&num_entries, buffer, ids).IsSuccess());
    REQUIRE(num_entries == size_entries);

    std::vector<Entry> entries;
    for (u32 i = 0; i <= num_entries; i++) {
        ssl::BuiltInCertificateInfo info;
        std::memcpy(&info, buffer.data() + i * sizeof(info), sizeof(info));
        if (i == num_entries) {
            CHECK(info.cert_id == CaCertificateId::All);
            CHECK(info.status == TrustedCertStatus::Invalid);
            break;
        }
        REQUIRE(info.der_offset + info.der_size <= buffer.size());
        const auto der = buffer.begin() + static_cast<std::ptrdiff_t>(info.der_offset);
        entries.push_back({info.cert_id, info.status,
                           std::vector<u8>(der, der + static_cast<std::ptrdiff_t>(info.der_size))});
    }
    return entries;
}

const Entry NintendoCa{CaCertificateId::NintendoCAG3, TrustedCertStatus::EnabledTrusted, {1, 2, 3}};
const Entry ThawteCa{CaCertificateId::thawtePrimaryRootCA, TrustedCertStatus::EnabledNotTrusted,
                     {4, 5}};

} // namespace

TEST_CASE("The private server CA is a trusted certificate even without firmware", "[core][ssl]") {
    const std::vector<u8> der = IsrgRootX2Der();
    const TemporaryFile ca_file{der};
    const PrivateServerCaSettings settings{true, ca_file.Path()};
    CertStore store{nullptr};

    const Entry ca{CertStore::PrivateServerCaId, TrustedCertStatus::EnabledTrusted, der};
    CHECK(GetCertificates(store, {CaCertificateId::All}) == std::vector{ca});
    // Titles ask for the ids they know, which the private server CA is added to
    CHECK(GetCertificates(store, {CaCertificateId::NintendoCAG3,
                                  CaCertificateId::DigiCertGlobalRootCA}) == std::vector{ca});
    CHECK(GetCertificates(store, {CertStore::PrivateServerCaId}) == std::vector{ca});
    CHECK(GetCertificates(store, {}).empty());
    CHECK(GetCertificates(store, {static_cast<CaCertificateId>(0x7FFF0000)}).empty());
}

TEST_CASE("The private server CA follows the built-in certificates asked for", "[core][ssl]") {
    const std::vector<u8> der = IsrgRootX2Der();
    const TemporaryFile ca_file{der};
    const PrivateServerCaSettings settings{true, ca_file.Path()};
    CertStore store{MakeBuiltInCertificates({NintendoCa, ThawteCa})};

    const Entry ca{CertStore::PrivateServerCaId, TrustedCertStatus::EnabledTrusted, der};
    CHECK(GetCertificates(store, {CaCertificateId::All}) ==
          std::vector{NintendoCa, ThawteCa, ca});
    CHECK(GetCertificates(store, {CaCertificateId::thawtePrimaryRootCA}) ==
          std::vector{ThawteCa, ca});
    CHECK(GetCertificates(store, {CaCertificateId::AmazonRootCA1}) == std::vector{ca});
}

TEST_CASE("Without a valid private server CA, the built-in certificates are as they were",
          "[core][ssl]") {
    const std::vector<u8> der = IsrgRootX2Der();
    std::vector<u8> truncated = der;
    truncated.pop_back();
    std::vector<u8> two = der;
    two.insert(two.end(), der.begin(), der.end());
    const TemporaryFile valid{der};
    const TemporaryFile pem{Bytes(IsrgRootX2Pem)};
    const TemporaryFile truncated_file{truncated};
    const TemporaryFile two_file{two};
    const TemporaryFile empty_file{std::span<const u8>{}};

    const auto check_unchanged = [](bool enabled, const std::string& path) {
        const PrivateServerCaSettings settings{enabled, path};
        CertStore store{MakeBuiltInCertificates({NintendoCa})};
        CHECK(GetCertificates(store, {CaCertificateId::All}) == std::vector{NintendoCa});
        CHECK(GetCertificates(store, {CaCertificateId::NintendoCAG3}) == std::vector{NintendoCa});
        CHECK(GetCertificates(store, {CaCertificateId::AmazonRootCA1}).empty());
    };
    SECTION("Not enabled") {
        check_unchanged(false, valid.Path());
    }
    SECTION("No CA configured") {
        check_unchanged(true, "");
    }
    SECTION("A CA file missing") {
        check_unchanged(true, valid.Path() + ".missing");
    }
    SECTION("A CA file empty") {
        check_unchanged(true, empty_file.Path());
    }
    SECTION("A CA file in PEM") {
        check_unchanged(true, pem.Path());
    }
    SECTION("A CA file truncated") {
        check_unchanged(true, truncated_file.Path());
    }
    SECTION("A CA file with two certificates") {
        check_unchanged(true, two_file.Path());
    }
}
