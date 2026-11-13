// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/common_types.h"
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "common/settings_enums.h"

namespace UpdateChecker {

enum class Channel { Stable, ReleaseCandidate };

// A release tag: vX.Y.Z (stable) or vX.Y.Z-rcN (release candidate, N >= 1).
struct Version {
    std::array<u64, 3> core{};
    u64 rc{}; // 0 for a stable release

    // A stable release is newer than every release candidate of the same core.
    constexpr std::strong_ordering operator<=>(const Version& other) const {
        if (const auto order = core <=> other.core; order != 0) {
            return order;
        }
        if ((rc == 0) != (other.rc == 0)) {
            return rc == 0 ? std::strong_ordering::greater : std::strong_ordering::less;
        }
        return rc <=> other.rc;
    }
    constexpr bool operator==(const Version&) const = default;
};

// Numbers with leading zeroes (v01.2.3) are rejected.
constexpr std::optional<Version> ParseTag(std::string_view tag) {
    const auto number = [&tag](u64& out) {
        std::size_t digits = 0;
        out = 0;
        while (digits < tag.size() && tag[digits] >= '0' && tag[digits] <= '9') {
            if (out > (~u64{0} - 9) / 10) {
                return false;
            }
            out = out * 10 + static_cast<u64>(tag[digits] - '0');
            ++digits;
        }
        if (digits == 0 || (digits > 1 && tag[0] == '0')) {
            return false;
        }
        tag.remove_prefix(digits);
        return true;
    };
    const auto consume = [&tag](std::string_view prefix) {
        if (!tag.starts_with(prefix)) {
            return false;
        }
        tag.remove_prefix(prefix.size());
        return true;
    };

    Version version;
    if (!consume("v") || !number(version.core[0]) || !consume(".") || !number(version.core[1]) ||
        !consume(".") || !number(version.core[2])) {
        return std::nullopt;
    }
    if (!tag.empty()) {
        if (!consume("-rc") || !number(version.rc) || version.rc == 0 || !tag.empty()) {
            return std::nullopt;
        }
    }
    return version;
}

// The channel a build with this tag belongs to; nullopt when it is not a tagged release.
std::optional<Channel> ChannelOfTag(std::string_view tag);

// The channel of the running build.
std::optional<Channel> BuildChannel();

enum class Failure { None, NotReleaseBuild, RateLimited, Http, Network };

struct CheckResult {
    std::optional<Common::Net::Release> update;
    Failure failure{Failure::None};
    int status{}; // HTTP status of a failed request, or 0 without a response
};

// 403 and 429 are rate limiting, other HTTP errors are Http, and no response is Network.
Failure ClassifyFailure(int status);

// The newest release strictly newer than current_tag that the channel allows. When platform is
// not empty, releases without an asset for it are skipped. Does no I/O.
std::optional<Common::Net::Release> SelectUpdate(
    std::span<const Common::Net::Release> releases, std::string_view current_tag,
    Channel channel, std::string_view platform,
    Common::Net::LinuxPackage package = Common::Net::LinuxPackage::Any);

// The release asset platform to download for `variant`: `build_platform` with "-pgo" added or
// removed. Empty when the build has no release platform.
std::string DownloadPlatform(Settings::UpdateVariant variant,
                             std::string_view build_platform = Common::g_build_release_platform);

// Queries the releases of the running build's repository. `platform` is the one to download.
CheckResult Check(Channel channel, std::string_view platform = Common::g_build_release_platform);

std::optional<Common::Net::Release> GetUpdate();

} // namespace UpdateChecker
