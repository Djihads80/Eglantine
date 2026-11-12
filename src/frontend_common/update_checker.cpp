// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace UpdateChecker {

std::optional<Channel> ChannelOfTag(std::string_view tag) {
    const auto version = ParseTag(tag);
    if (!version) {
        return std::nullopt;
    }
    return version->rc == 0 ? Channel::Stable : Channel::ReleaseCandidate;
}

std::optional<Channel> BuildChannel() {
    return ChannelOfTag(Common::g_build_version);
}

Failure ClassifyFailure(int status) {
    if (status == 403 || status == 429) {
        return Failure::RateLimited;
    }
    return status >= 400 ? Failure::Http : Failure::Network;
}

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view current_tag, Channel channel,
                                                 std::string_view platform,
                                                 Common::Net::LinuxPackage package) {
    auto newest = ParseTag(current_tag);
    if (!newest) {
        return std::nullopt;
    }

    const Common::Net::Release* selected = nullptr;
    for (const auto& release : releases) {
        const auto version = ParseTag(release.tag);
        if (!version || *version <= *newest) {
            continue;
        }
        if (channel == Channel::Stable && version->rc != 0) {
            continue;
        }
        if (!platform.empty() && release.GetPlatformAssets(platform, package).empty()) {
            continue;
        }
        newest = version;
        selected = &release;
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

CheckResult Check(Channel channel) {
    if (!ParseTag(Common::g_build_version)) {
        return {.failure = Failure::NotReleaseBuild};
    }

    const auto list = Common::Net::GetReleases();
    if (!list.ok) {
        LOG_WARNING(Frontend, "Update check failed with status {}", list.status);
        return {.failure = ClassifyFailure(list.status), .status = list.status};
    }

    auto update = SelectUpdate(list.releases, Common::g_build_version, channel,
                               Common::g_build_release_platform,
                               Common::Net::CurrentLinuxPackage());
    if (update) {
        LOG_INFO(Frontend, "Received update {}", update->title);
    }
    return {.update = std::move(update)};
}

std::optional<Common::Net::Release> GetUpdate() {
    const auto channel = BuildChannel();
    return channel ? Check(*channel).update : std::nullopt;
}

} // namespace UpdateChecker
