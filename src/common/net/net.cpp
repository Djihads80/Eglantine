// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <optional>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <cpr/cpr.h>
#include <glaze/glaze.hpp>

#include "common/scm_rev.h"
#include "net.h"

#include "common/logging.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#ifdef _WIN32
#include <filesystem>
#endif

#define QT_TR_NOOP(x) x

namespace Common::Net {

std::vector<NamedAsset> GetPlatformAssets(const Release& r) {
    // TODO(crueter): Need better handling for this as a whole.
#ifdef NIGHTLY_BUILD
    std::vector<std::string> result;
    boost::algorithm::split(result, tag, boost::is_any_of("."));
    if (result.size() != 2)
        return {};
    const auto ref = result.at(1);
#else
    const auto ref = r.tag_name;
#endif

    std::vector<NamedAsset> found_assets;

    [[maybe_unused]] const auto find_asset =
        [&r, &found_assets](const std::string& name, const std::vector<std::string>& suffixes) {
            const auto& assets = r.assets;
            for (const auto& s : suffixes) {
                const auto it = std::ranges::find_if(
                    assets, [&s](const Asset& a) -> bool {
                        return a.name.ends_with(s);
                    });

                if (it != assets.end()) {
                    auto asset = Asset(*it);
                    found_assets.emplace_back(NamedAsset{.name = name, .asset = asset});
                }
            }
        };

#ifdef _WIN32
    // system.txt is (will be) installed in the same directory as the installed executable
    const auto is_system = std::filesystem::exists(std::filesystem::current_path() / "system.txt");
    std::string ext;
    if (is_system) {
        ext = ".exe";
    } else {
        ext = ".zip";
    }

#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
    find_asset("Standard", {std::format("amd64-msvc-standard.{}", ext)});
#else  // _MSC_VER
    find_asset("Standard", {std::format(BUILD_ID "-gcc-standard.{}", ext)});
    find_asset("PGO", {std::format(BUILD_ID "-clang-pgo.{}", ext)});
#endif // _MSC_VER
#elif defined(ARCHITECTURE_arm64)
    find_asset("Standard", {std::format("arm64-clang-standard.{}", ext)});
    find_asset("PGO", {std::format("arm64-clang-pgo.{}", ext)});
#endif // ARCHITECTURE_arm64
#elif defined(__APPLE__)
#ifdef ARCHITECTURE_arm64
    find_asset("Standard", {"standard.dmg", "standard.tar.gz", ".dmg", ".tar.gz"});
    find_asset("PGO", {"pgo.dmg", "pgo.tar.gz"});
#endif // ARCHITECTURE_arm64
#elif defined(__ANDROID__)
#ifdef ARCHITECTURE_x86_64
    find_asset("Standard", {"chromeos.apk"});
#elif defined(ARCHITECTURE_arm64)
#ifdef YUZU_LEGACY
    find_asset("Standard", {"legacy.apk"});
#elif defined(GENSHIN_SPOOF)
    find_asset("Standard", {"optimized.apk"});
#else
    find_asset("Standard", {"standard.apk"});
#endif // GENSHIN_SPOOF
#endif // ARCHITECTURE_arm64
#endif // __APPLE__
    return found_assets;
}

std::optional<std::string> MakeRequest(const std::string& url) {
    cpr::SslOptions opts;
#ifdef YUZU_BUNDLED_OPENSSL
    opts = cpr::Ssl(cpr::ssl::CaBuffer{std::string{kCert}});
#endif

    cpr::Response res = cpr::Get(cpr::Url{url}, opts);

    if (res.error) {
        LOG_ERROR(Frontend, "Failed to download {}: {}", url, res.error.message);
        return std::nullopt;
    } else if (res.status_code < 200 || res.status_code >= 300) {
        LOG_ERROR(Frontend, "Received status code {} for {}", res.status_code, url);
        return std::nullopt;
    }

    return res.text;
}

std::optional<std::string> MakeRequest(const std::string_view url) {
    return MakeRequest(std::string(url));
}

std::vector<Release> GetReleases() {
    const auto body = GetReleasesBody();

    if (!body) {
        LOG_WARNING(Common, "Failed to get stable releases");
        return {};
    }

    return GetReleasesFromJson(body.value());
}

std::optional<Release> GetLatestRelease() {
    const auto releases_path = Common::g_build_auto_update_api_path;
    const auto url = std::format("https://{}{}", std::string{Common::g_build_auto_update_api}, releases_path);

    const auto body = MakeRequest(url);
    if (!body) {
        LOG_WARNING(Common, "Failed to get latest release");
        return std::nullopt;
    }

    const std::string_view body_str = body.value();
    Release release;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(release, body_str);

    if (ec) {
        LOG_CRITICAL(Common, "Latest Release JSON parse error: {}",
                     glz::format_error(ec, body_str));
        return std::nullopt;
    }

    return release;
}

std::optional<std::string> GetReleasesBody() {
    const auto releases_path =
        std::format("/{}/{}/releases", std::string{Common::g_build_auto_update_stable_api_path},
                    std::string{Common::g_build_auto_update_stable_repo});
    const auto url = std::format("https://{}{}", std::string{Common::g_build_auto_update_stable_api}, releases_path);

    return MakeRequest(url);
}

std::vector<Release> GetReleasesFromJson(const std::string& body) {
    std::vector<Release> releases;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(releases, body);

    if (ec) {
        LOG_CRITICAL(Common, "Release JSON parse error: {}", glz::format_error(ec, body));
        return {};
    }

    return releases;
}

} // namespace Common::Net
