// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <boost/algorithm/string/replace.hpp>

#include <fmt/format.h>
#include "common/scm_rev.h"
#include "net.h"

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

#include "common/httplib.h"

#include "common/net/ca_store.h"

namespace Common::Net {

#ifdef YUZU_BUNDLED_OPENSSL
std::string BundledCaFile() {
    namespace fs = std::filesystem;
    try {
        const std::string_view contents{kCert, sizeof(kCert) - 1};
        const auto matches = [&contents](const fs::path& file) {
            std::ifstream stream{file, std::ios::binary};
            const std::string on_disk{std::istreambuf_iterator<char>{stream}, {}};
            return stream && on_disk == contents;
        };

        const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
        const auto path = dir / "ca-bundle.pem";
        if (matches(path)) {
            return Common::FS::PathToUTF8String(path);
        }

        // Written beside it and renamed, so another instance never reads half a file.
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto temp = fs::path{path}.concat(
            fmt::format(".{}", std::chrono::steady_clock::now().time_since_epoch().count()));
        {
            std::ofstream stream{temp, std::ios::binary | std::ios::trunc};
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!stream) {
                fs::remove(temp, ec);
                return {};
            }
        }
        fs::rename(temp, path, ec);
        if (ec) {
            fs::remove(temp, ec);
        }
        // A failed rename is fine when another instance has just written the same file.
        return matches(path) ? Common::FS::PathToUTF8String(path) : std::string{};
    } catch (const std::exception& e) {
        LOG_WARNING(Common, "Could not write the built-in CA bundle: {}", e.what());
        return {};
    }
}
#endif

LinuxPackage CurrentLinuxPackage() {
#ifdef __linux__
    const char* appimage = std::getenv("APPIMAGE");
    return appimage && *appimage ? LinuxPackage::AppImage : LinuxPackage::Archive;
#else
    return LinuxPackage::Any;
#endif
}

std::vector<Asset> Release::GetPlatformAssets() const {
    return GetPlatformAssets(Common::g_build_release_platform, CurrentLinuxPackage());
}

std::vector<Asset> Release::GetPlatformAssets(std::string_view platform,
                                              LinuxPackage package) const {
    const bool pgo = platform.ends_with("-pgo");
    if (pgo) {
        platform.remove_suffix(4);
    }

    std::vector<std::string_view> extensions;
    if (platform == "linux-x86_64" || platform == "linux-aarch64") {
        if (package != LinuxPackage::Archive) {
            extensions.push_back(".AppImage");
        }
        if (package != LinuxPackage::AppImage) {
            extensions.push_back(".tar.gz");
        }
    } else if (platform == "windows-x86_64") {
        extensions = {".zip"};
    } else if (platform == "macos-arm64") {
        extensions = {".dmg", ".zip"};
    } else {
        return {};
    }

    for (const std::string_view variant : pgo ? std::vector<std::string_view>{"-pgo", ""}
                                              : std::vector<std::string_view>{""}) {
        for (const auto extension : extensions) {
            const auto filename = fmt::format("eglantine-{}{}{}", platform, variant, extension);
            const auto suffix = fmt::format("{}/{}", base_download_url, filename);
            const auto it = std::ranges::find_if(
                assets, [&suffix](const std::string& url) { return url.ends_with(suffix); });
            if (it == assets.end()) {
                continue;
            }

            // Split the download URL into the host and path an httplib client expects.
            const auto scheme_end = it->find("://");
            const auto path_start =
                it->find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
            Asset asset{.name = variant.empty() ? "Standard" : "PGO",
                        .url = it->substr(0, path_start),
                        .path = it->substr(path_start),
                        .filename = filename};
            if (const auto info = asset_info.find(*it); info != asset_info.end()) {
                asset.sha256 = info->second.sha256;
                asset.size = info->second.size;
            }
            return {std::move(asset)};
        }
    }
    return {};
}

static bool IsCommitSha(std::string_view text) {
    return text.size() == 40 && std::ranges::all_of(text, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

static inline u64 ParseIsoTimestamp(const std::string& iso) {
    if (iso.empty())
        return 0;

    std::string buf = iso;
    if (buf.back() == 'Z')
        buf.pop_back();

    std::tm tm{};
    std::istringstream ss(buf);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail())
        return 0;

#ifdef _WIN32
    return static_cast<u64>(_mkgmtime(&tm));
#else
    return static_cast<u64>(timegm(&tm));
#endif
}

std::optional<Release> Release::FromJson(const nlohmann::json& json, const std::string& host,
                                         const std::string& repo) {
    Release rel;
    if (!json.is_object())
        return std::nullopt;

    if (json.value("draft", false))
        return std::nullopt;

    rel.tag = json.value("tag_name", std::string{});
    if (rel.tag.empty())
        return std::nullopt;

    rel.title = json.contains("name") && json["name"].is_string()
                    ? json["name"].get<std::string>()
                    : rel.tag;
    rel.id = json.value("id", std::hash<std::string>{}(rel.title));

    rel.published = ParseIsoTimestamp(json.value("published_at", std::string{}));
    rel.prerelease = json.value("prerelease", false);

    const auto target = json.value("target_commitish", std::string{});
    if (IsCommitSha(target)) {
        rel.commit = target;
    }

    auto body = json.contains("body") && json["body"].is_string()
                    ? json["body"].get<std::string>()
                    : rel.title;
    boost::replace_all(body, "\\r", "");
    boost::replace_all(body, "\\n", "\n");
    rel.body = body;

    rel.host = host;

    const auto release_base =
        fmt::format("{}/{}/releases", Common::g_build_auto_update_website, repo);
    const auto fallback_html = fmt::format("{}/tag/{}", release_base, rel.tag);
    rel.html_url = json.value("html_url", fallback_html);

    rel.base_download_url = fmt::format("/{}/releases/download/{}", repo, rel.tag);

    const auto assets = json.value("assets", nlohmann::json::array());
    if (!assets.is_array()) {
        return std::nullopt;
    }
    for (const auto& asset : assets) {
        const auto url = asset.value("browser_download_url", std::string{});
        rel.assets.emplace_back(url);

        AssetInfo info;
        if (asset.contains("size") && asset["size"].is_number_unsigned()) {
            info.size = asset["size"].get<u64>();
        }
        constexpr std::string_view digest_prefix = "sha256:";
        if (asset.contains("digest") && asset["digest"].is_string()) {
            const auto digest = asset["digest"].get<std::string>();
            const std::string_view hex = digest.starts_with(digest_prefix)
                                             ? std::string_view{digest}.substr(digest_prefix.size())
                                             : std::string_view{};
            if (hex.size() == 64 &&
                std::ranges::all_of(hex, [](unsigned char ch) { return std::isxdigit(ch); })) {
                info.sha256 = std::string{hex};
                std::ranges::transform(info.sha256, info.sha256.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            }
        }
        rel.asset_info.emplace(url, std::move(info));
    }

    return rel;
}

std::optional<Release> Release::FromJson(const std::string_view& json, const std::string& host,
                                         const std::string& repo) {
    try {
        return FromJson(nlohmann::json::parse(json), host, repo);
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return {};
}

std::vector<Release> Release::ListFromJson(const nlohmann::json& json, const std::string& host,
                                           const std::string& repo) {
    if (!json.is_array())
        return {};

    std::vector<Release> releases;
    for (const auto& obj : json) {
        auto rel = Release::FromJson(obj, host, repo);
        if (rel)
            releases.emplace_back(rel.value());
    }
    return releases;
}

std::vector<Release> Release::ListFromJson(const std::string_view& json, const std::string& host,
                                           const std::string& repo) {
    try {
        return ListFromJson(nlohmann::json::parse(json), host, repo);
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return {};
}

RequestResult Fetch(const std::string& url, const std::string& path) {
    try {
        constexpr std::size_t timeout_seconds = 15;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(url);
        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

        Common::Net::ConfigureCaStore(*client);

        if (client == nullptr) {
            LOG_ERROR(Common, "Invalid URL {}{}", url, path);
            return {};
        }

        httplib::Request request{
            .method = "GET",
            .path = path,
            .headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "Eglantine"}},
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
            return {.status = response.status};
        }
        if (!response.has_header("content-type")) {
            LOG_ERROR(Common, "GET to {}{} returned no content", url, path);
            return {.status = response.status};
        }

        return {.body = response.body, .status = response.status};
    } catch (std::exception& e) {
        LOG_ERROR(Common, "GET to {}{} failed during update check: {}", url, path, e.what());
        return {};
    }
}

std::optional<std::string> MakeRequest(const std::string& url, const std::string& path) {
    return Fetch(url, path).body;
}

namespace {

struct ReleasesJson {
    nlohmann::json releases = nlohmann::json::array();
    bool ok{};
    int status{};
};

// GitHub lists releases newest first by creation, and rolling builds are recreated on every build,
// so tagged releases can sit several pages down. Every page is read: a list cut short could hide
// the newest tag.
ReleasesJson FetchReleasesJson() {
    constexpr int per_page = 100;
    constexpr int max_pages = 10;
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);

    ReleasesJson result;
    for (int page = 1; page <= max_pages; ++page) {
        const auto response =
            Fetch(url, fmt::format("/repos/{}/releases?per_page={}&page={}",
                                   Common::g_build_auto_update_repo, per_page, page));
        result.status = response.status;
        if (!response.body) {
            LOG_WARNING(Common, "Failed to get page {} of the releases", page);
            return {.status = response.status};
        }
        // A body that is not a release list, such as a proxy's error page, fails the check instead
        // of reading as one that found no update.
        auto json = nlohmann::json::parse(*response.body, nullptr, false);
        if (!json.is_array()) {
            LOG_WARNING(Common, "Releases response is not a JSON array");
            return {.status = response.status};
        }
        const auto count = json.size();
        for (auto& release : json) {
            result.releases.push_back(std::move(release));
        }
        if (count < static_cast<std::size_t>(per_page)) {
            result.ok = true;
            return result;
        }
    }
    LOG_WARNING(Common, "More than {} pages of releases, reading only those", max_pages);
    result.ok = true;
    return result;
}

} // namespace

TagList GetTags(std::string_view prefix) {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto response = Fetch(url, fmt::format("/repos/{}/git/matching-refs/tags/{}",
                                                 Common::g_build_auto_update_repo, prefix));
    if (!response.body) {
        return {.status = response.status};
    }
    const auto json = nlohmann::json::parse(*response.body, nullptr, false);
    if (!json.is_array()) {
        LOG_WARNING(Common, "Tags response is not a JSON array");
        return {.status = response.status};
    }

    constexpr std::string_view ref_prefix = "refs/tags/";
    TagList result{.ok = true, .status = response.status};
    for (const auto& ref : json) {
        if (!ref.is_object() || !ref.contains("ref") || !ref["ref"].is_string()) {
            continue;
        }
        const auto name = ref["ref"].get<std::string>();
        if (!name.starts_with(ref_prefix)) {
            continue;
        }
        Tag tag{.name = name.substr(ref_prefix.size())};
        const auto object = ref.value("object", nlohmann::json::object());
        if (object.is_object() && object.value("type", std::string{}) == "commit") {
            const auto sha = object.value("sha", std::string{});
            if (IsCommitSha(sha)) {
                tag.commit = sha;
            }
        }
        result.tags.push_back(std::move(tag));
    }
    return result;
}

ReleaseResult GetRelease(std::string_view tag) {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto response = Fetch(
        url, fmt::format("/repos/{}/releases/tags/{}", Common::g_build_auto_update_repo, tag));
    if (!response.body) {
        return {.status = response.status};
    }
    const auto json = nlohmann::json::parse(*response.body, nullptr, false);
    if (!json.is_object()) {
        LOG_WARNING(Common, "Release response is not a JSON object");
        return {.status = response.status};
    }
    try {
        return {.release = Release::FromJson(json, url, Common::g_build_auto_update_repo),
                .ok = true,
                .status = response.status};
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse release {}: {}", tag, e.what());
        return {.status = response.status};
    }
}

std::optional<std::string> GetReleasesBody() {
    const auto json = FetchReleasesJson();
    if (!json.ok) {
        return std::nullopt;
    }
    return json.releases.dump();
}

} // namespace Common::Net
