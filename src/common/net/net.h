// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Common::Net {

struct Asset {
    std::string name;
    std::string url;
    std::string path;
    std::string filename;
    // Lowercase hex SHA-256 and byte size from the release API; empty and zero when unpublished.
    std::string sha256;
    u64 size{};
};

struct AssetInfo {
    std::string sha256;
    u64 size{};
};

// How a Linux build is installed, which decides between the AppImage and the tar.gz.
enum class LinuxPackage { Any, AppImage, Archive };

LinuxPackage CurrentLinuxPackage();

struct Release {
    std::string title;
    std::string body;
    std::string tag;
    std::string base_download_url;
    std::string html_url;
    std::string host;
    // The commit the release was made for: 40 lowercase hex characters, or empty when the release
    // targets a branch name or says nothing.
    std::string target_commit;

    std::vector<std::string> assets;
    // Keyed by the asset's browser_download_url.
    std::unordered_map<std::string, AssetInfo> asset_info;

    u64 id;
    u64 published;
    bool prerelease;

    // Get the relevant list of assets for the current platform. A PGO platform prefers its -pgo
    // asset and falls back to the standard one; a standard platform only gets the standard asset.
    std::vector<Asset> GetPlatformAssets() const;
    std::vector<Asset> GetPlatformAssets(std::string_view platform,
                                         LinuxPackage package = LinuxPackage::Any) const;

    static std::optional<Release> FromJson(const nlohmann::json& json, const std::string &host, const std::string& repo);
    static std::optional<Release> FromJson(const std::string_view& json, const std::string &host, const std::string& repo);
    static std::vector<Release> ListFromJson(const nlohmann::json &json, const std::string &host, const std::string &repo);
    static std::vector<Release> ListFromJson(const std::string_view &json, const std::string &host, const std::string &repo);
};

// The most recently published release made for exactly `commit` that has an asset for `platform`.
std::optional<Release> FindReleaseForCommit(const std::vector<Release>& releases,
                                            std::string_view commit, std::string_view platform,
                                            LinuxPackage package = LinuxPackage::Any);

struct RequestResult {
    std::optional<std::string> body;
    // HTTP status, or 0 when no response arrived.
    int status{};
};

// Make a request via httplib, and return the response body and HTTP status.
RequestResult Fetch(const std::string &url, const std::string &path);

// Make a request via httplib, and return the response body if applicable.
std::optional<std::string> MakeRequest(const std::string &url, const std::string &path);

struct ReleaseList {
    std::vector<Release> releases;
    // False when the request failed; status is then the HTTP status, or 0 without a response.
    bool ok{};
    int status{};
};

// Get every release, including prereleases, reading all pages of the list.
ReleaseList GetReleases();

// Get every release as the text of one JSON array.
std::optional<std::string> GetReleasesBody();

// Newest commit of a branch or pull request.
struct RemoteHead {
    // Lowercase 40-character commit hash.
    std::string sha;
    // Pull requests only.
    std::string title;
    std::string author;
    bool open{true};
    bool merged{};

    // From GET /repos/{repo}/branches/{branch}
    static std::optional<RemoteHead> FromBranchJson(const std::string_view& json);
    // From GET /repos/{repo}/pulls/{number}
    static std::optional<RemoteHead> FromPullJson(const std::string_view& json);
};

struct RemoteHeadResult {
    std::optional<RemoteHead> head;
    // HTTP status, or 0 when no response arrived.
    int status{};
};

RemoteHeadResult GetBranchHead(const std::string& branch);
RemoteHeadResult GetPullRequestHead(u64 number);

// Branch names from GET /repos/{repo}/branches
std::vector<std::string> BranchNamesFromJson(const std::string_view& json);

// Branches from GET /repos/{repo}/activity, the most recently pushed first, at most `limit`.
// Branches deleted since their last push are left out.
std::vector<std::string> RecentBranchesFromActivityJson(const std::string_view& json,
                                                        std::size_t limit);

struct BranchList {
    std::vector<std::string> names;
    bool ok{};
    int status{};
};

// The most recently pushed branches, or every branch name when the activity cannot be read.
BranchList GetBranches();

}
