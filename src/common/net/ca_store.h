// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace Common::Net {

/// Path of the host's CA bundle, or empty when SSL_CERT_FILE/SSL_CERT_DIR is set (OpenSSL then
/// handles it) or no known bundle exists. OpenSSL's compiled-in default path is the build
/// machine's layout, which is wrong on other distros.
inline std::string FindSystemCaBundle() {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (std::getenv("SSL_CERT_FILE") != nullptr || std::getenv("SSL_CERT_DIR") != nullptr) {
        return {};
    }
    constexpr std::array candidates{
        "/etc/ssl/certs/ca-certificates.crt",     // Debian, Ubuntu, Arch, SteamOS, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",       // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                 // openSUSE
        "/etc/ssl/cert.pem",                      // Alpine, BSD
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ca-certificates/extracted/tls-ca-bundle.pem",
    };
    for (const char* path : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
    }
#endif
    return {};
}

/// Trust the host's CA bundle, else the built-in one when this build has it.
template <typename Client>
void ConfigureCaStore(Client& client) {
    static const std::string system_bundle = FindSystemCaBundle();
    if (!system_bundle.empty()) {
        client.set_ca_cert_path(system_bundle);
        return;
    }
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

} // namespace Common::Net
