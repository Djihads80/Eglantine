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

// Get the latest releases, including prereleases.
ReleaseList GetReleases();

// Get the latest releases as text.
std::optional<std::string> GetReleasesBody();

}
