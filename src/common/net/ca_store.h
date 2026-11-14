// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

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
/// machine's layout, which is wrong on other distros, and on macOS points into Homebrew.
inline std::string FindSystemCaBundle() {
#if !defined(_WIN32)
    if (std::getenv("SSL_CERT_FILE") != nullptr || std::getenv("SSL_CERT_DIR") != nullptr) {
        return {};
    }
    constexpr std::array candidates{
        "/etc/ssl/certs/ca-certificates.crt",     // Debian, Ubuntu, Arch, SteamOS, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",       // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                 // openSUSE
        "/etc/ssl/cert.pem",                      // Alpine, BSD, macOS
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

#ifdef YUZU_BUNDLED_OPENSSL
/// The built-in CA bundle written to the cache folder, or empty when that fails. httplib hands a
/// CA file on to the client it makes for a redirect to another host, but not a store loaded from
/// memory.
std::string BundledCaFile();
#endif

/// Trust the host's CA bundle, else the built-in one when this build has it.
template <typename Client>
void ConfigureCaStore(Client& client) {
    static const std::string system_bundle = FindSystemCaBundle();
    if (!system_bundle.empty()) {
        client.set_ca_cert_path(system_bundle);
        return;
    }
#ifdef YUZU_BUNDLED_OPENSSL
    static const std::string bundled_file = BundledCaFile();
    if (!bundled_file.empty()) {
        client.set_ca_cert_path(bundled_file);
        return;
    }
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

} // namespace Common::Net
