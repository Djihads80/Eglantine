// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <optional>
#include <variant>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <glaze/glaze.hpp>

#include <fmt/format.h>
#include "common/scm_rev.h"
#include "net.h"

#include "common/logging.h"

#include "common/httplib.h"
#include <print>

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#define QT_TR_NOOP(x) x

namespace Common::Net {

std::vector<NamedAsset> GetPlatformAssets(const Release &r) {
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

    [[maybe_unused]] const auto find_asset = [&r, &found_assets](const std::string &name, const std::vector<std::string>& suffixes) {
        const auto &assets = r.assets;
        for (const auto &s : suffixes) {
            const auto it = std::ranges::find_if(
                assets, [&s](const std::variant<std::string, Asset>& a) -> bool {
                    return std::holds_alternative<Asset>(a) && std::get<Asset>(a).name.ends_with(s);
                });

            if (it != assets.end()) {
                auto asset = Asset(*it);
                found_assets.emplace_back(NamedAsset{
                    .name = name,
                    .asset = asset
                });
            }
        }
    };

#ifdef _WIN32
#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
    find_asset("Standard", {"amd64-msvc-standard.exe", "amd64-msvc-standard.zip"});
#else // _MSC_VER
    find_asset("Standard", {BUILD_ID "-gcc-standard.exe", BUILD_ID "-gcc-standard.zip"});
    find_asset("PGO", {BUILD_ID "-clang-pgo.exe", BUILD_ID "-clang-pgo.zip"});
#endif // _MSC_VER
#elif defined(ARCHITECTURE_arm64)
    find_asset("Standard", {"arm64-clang-standard.exe", "arm64-clang-standard.zip"});
    find_asset("PGO", {"arm64-clang-pgo.exe", "arm64-clang-pgo.zip"});
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

std::optional<std::string> MakeRequest(const std::string& url, const std::string& path) {
    try {
        constexpr std::size_t timeout_seconds = 15;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(url);
        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

        if (client == nullptr) {
            LOG_ERROR(Common, "Invalid URL {}{}", url, path);
            return {};
        }

        httplib::Request request{
            .method = "GET",
            .path = path,
        };

        client->set_follow_location(true);
        httplib::Result result = client->send(request);

        if (!result) {
            LOG_ERROR(Common, "GET to {}{} returned null", url, path);
            return {};
        }

        const auto& response = result.value();
        if (response.status >= 400) {
            LOG_ERROR(Common, "GET to {}{} returned error status code: {}", url, path,
                      response.status);
            return {};
        }
        if (!response.has_header("content-type")) {
            LOG_ERROR(Common, "GET to {}{} returned no content", url, path);
            return {};
        }

        return response.body;
    } catch (std::exception& e) {
        LOG_ERROR(Common, "GET to {}{} failed during update check: {}", url, path, e.what());
        return std::nullopt;
    }
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
    const auto releases_path =  Common::g_build_auto_update_api_path;
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);

    const auto body = MakeRequest(url, releases_path);
    if (!body) {
        LOG_WARNING(Common, "Failed to get latest release");
        return std::nullopt;
    }

    const std::string_view body_str = body.value();
    Release release;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(release, body_str);

    if (ec) {
        LOG_CRITICAL(Common, "Latest Release JSON parse error: {}", glz::format_error(ec, body_str));
        return std::nullopt;
    }

    return release;

}

std::optional<std::string> GetReleasesBody() {
    const auto releases_path =
        fmt::format("/{}/{}/releases", Common::g_build_auto_update_stable_api_path,
                    Common::g_build_auto_update_stable_repo);
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_stable_api);

    return MakeRequest(url, releases_path);
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
