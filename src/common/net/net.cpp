// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <boost/algorithm/string/replace.hpp>

#include <fmt/format.h>
#include "common/scm_rev.h"
#include "net.h"

#include "common/logging.h"

#include "common/httplib.h"

#include "common/net/ca_store.h"

namespace Common::Net {

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

    const std::string prefix = fmt::format("/{}/releases/download/{}/",
                                           Common::g_build_auto_update_repo, tag);
    for (const std::string_view variant : pgo ? std::vector<std::string_view>{"-pgo", ""}
                                              : std::vector<std::string_view>{""}) {
        for (const auto extension : extensions) {
            const auto filename = fmt::format("eglantine-{}{}{}", platform, variant, extension);
            const auto it = std::ranges::find_if(assets, [&](const std::string& url) {
                return url.ends_with(prefix + filename);
            });
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

// `text` lowercased when it is a 40-character commit hash, else empty.
static std::string LowercaseSha(std::string text) {
    if (text.size() != 40 ||
        !std::ranges::all_of(text, [](unsigned char ch) { return std::isxdigit(ch); })) {
        return {};
    }
    std::ranges::transform(text, text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
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

    auto body = json.contains("body") && json["body"].is_string()
                    ? json["body"].get<std::string>()
                    : rel.title;
    boost::replace_all(body, "\\r", "");
    boost::replace_all(body, "\\n", "\n");
    rel.body = body;

    rel.host = host;
    rel.target_commit = LowercaseSha(json.value("target_commitish", std::string{}));

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

std::optional<Release> FindReleaseForCommit(const std::vector<Release>& releases,
                                            std::string_view commit, std::string_view platform,
                                            LinuxPackage package) {
    if (commit.size() != 40) {
        return std::nullopt;
    }
    const auto wanted = LowercaseSha(std::string{commit});
    if (wanted.empty()) {
        return std::nullopt;
    }
    std::optional<Release> best;
    for (const auto& release : releases) {
        if (release.target_commit != wanted || release.GetPlatformAssets(platform, package).empty()) {
            continue;
        }
        if (!best || release.published > best->published) {
            best = release;
        }
    }
    return best;
}

static std::string ReleasesPath() {
    return fmt::format("/repos/{}/releases?per_page=100", Common::g_build_auto_update_repo);
}

ReleaseList GetReleases() {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto response = Fetch(url, ReleasesPath());

    if (!response.body) {
        LOG_WARNING(Common, "Failed to get releases");
        return {.status = response.status};
    }

    return {.releases = Release::ListFromJson(std::string_view{*response.body}, url,
                                              Common::g_build_auto_update_repo),
            .ok = true,
            .status = response.status};
}

std::optional<std::string> GetReleasesBody() {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    return MakeRequest(url, ReleasesPath());
}

static std::string StringAt(const nlohmann::json& json, const char* key) {
    return json.contains(key) && json[key].is_string() ? json[key].get<std::string>()
                                                       : std::string{};
}

// The commit hash in an object with a "sha" key, lowercased, when it is 40 hex characters.
static std::optional<std::string> CommitSha(const nlohmann::json& commit) {
    if (!commit.contains("sha") || !commit["sha"].is_string())
        return std::nullopt;

    auto sha = commit["sha"].get<std::string>();
    if (sha.size() != 40 ||
        !std::ranges::all_of(sha, [](unsigned char ch) { return std::isxdigit(ch); })) {
        return std::nullopt;
    }
    std::ranges::transform(sha, sha.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return sha;
}

std::optional<RemoteHead> RemoteHead::FromBranchJson(const std::string_view& json) {
    try {
        const auto parsed = nlohmann::json::parse(json);
        if (!parsed.contains("commit"))
            return std::nullopt;

        const auto sha = CommitSha(parsed["commit"]);
        if (!sha)
            return std::nullopt;

        return RemoteHead{.sha = *sha};
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return std::nullopt;
}

std::optional<RemoteHead> RemoteHead::FromPullJson(const std::string_view& json) {
    try {
        const auto parsed = nlohmann::json::parse(json);
        if (!parsed.contains("head"))
            return std::nullopt;

        const auto sha = CommitSha(parsed["head"]);
        if (!sha)
            return std::nullopt;

        RemoteHead head{.sha = *sha};
        head.title = StringAt(parsed, "title");
        head.author = parsed.contains("user") ? StringAt(parsed["user"], "login") : std::string{};
        head.open = StringAt(parsed, "state") == "open";
        head.merged = parsed.contains("merged") && parsed["merged"].is_boolean() &&
                      parsed["merged"].get<bool>();
        return head;
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return std::nullopt;
}

static bool IsUnreserved(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

// Percent-encode a branch name for a URL path, keeping the slashes between its segments.
static std::string EncodeBranch(const std::string& branch) {
    std::string encoded;
    for (const auto ch : branch) {
        const auto uch = static_cast<unsigned char>(ch);
        if (IsUnreserved(uch) || ch == '/') {
            encoded.push_back(ch);
        } else {
            encoded += fmt::format("%{:02X}", uch);
        }
    }
    return encoded;
}

RemoteHeadResult GetBranchHead(const std::string& branch) {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto path = fmt::format("/repos/{}/branches/{}", Common::g_build_auto_update_repo,
                                  EncodeBranch(branch));
    const auto response = Fetch(url, path);

    if (!response.body) {
        return {.status = response.status};
    }

    return {.head = RemoteHead::FromBranchJson(std::string_view{*response.body}),
            .status = response.status};
}

RemoteHeadResult GetPullRequestHead(u64 number) {
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto path =
        fmt::format("/repos/{}/pulls/{}", Common::g_build_auto_update_repo, number);
    const auto response = Fetch(url, path);

    if (!response.body) {
        return {.status = response.status};
    }

    return {.head = RemoteHead::FromPullJson(std::string_view{*response.body}),
            .status = response.status};
}

std::vector<std::string> BranchNamesFromJson(const std::string_view& json) {
    std::vector<std::string> names;
    try {
        const auto parsed = nlohmann::json::parse(json);
        if (!parsed.is_array())
            return {};

        for (const auto& branch : parsed) {
            if (branch.contains("name") && branch["name"].is_string()) {
                names.push_back(branch["name"].get<std::string>());
            }
        }
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
        return {};
    }

    return names;
}

std::vector<std::string> RecentBranchesFromActivityJson(const std::string_view& json,
                                                        std::size_t limit) {
    constexpr std::string_view prefix = "refs/heads/";
    std::vector<std::string> names;
    std::vector<std::string> seen;
    try {
        const auto parsed = nlohmann::json::parse(json);
        if (!parsed.is_array())
            return {};

        // Newest first: a branch's first entry tells whether it still exists.
        for (const auto& activity : parsed) {
            if (names.size() >= limit)
                break;
            if (!activity.contains("ref") || !activity["ref"].is_string())
                continue;
            const auto ref = activity["ref"].get<std::string>();
            if (!ref.starts_with(prefix))
                continue;
            auto name = ref.substr(prefix.size());
            if (std::find(seen.begin(), seen.end(), name) != seen.end())
                continue;
            seen.push_back(name);
            const bool deleted = activity.contains("activity_type") &&
                                 activity["activity_type"] == "branch_deletion";
            if (!deleted)
                names.push_back(std::move(name));
        }
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
        return {};
    }

    return names;
}

BranchList GetBranches() {
    constexpr std::size_t RecentBranchCount = 15;
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);
    const auto activity = Fetch(
        url, fmt::format("/repos/{}/activity?per_page=100", Common::g_build_auto_update_repo));
    if (activity.body) {
        auto names = RecentBranchesFromActivityJson(*activity.body, RecentBranchCount);
        if (!names.empty()) {
            return {.names = std::move(names), .ok = true, .status = activity.status};
        }
    }

    const auto path =
        fmt::format("/repos/{}/branches?per_page=100", Common::g_build_auto_update_repo);
    const auto response = Fetch(url, path);

    if (!response.body) {
        LOG_WARNING(Common, "Failed to get branches");
        return {.status = response.status};
    }

    return {.names = BranchNamesFromJson(std::string_view{*response.body}),
            .ok = true,
            .status = response.status};
}

} // namespace Common::Net
