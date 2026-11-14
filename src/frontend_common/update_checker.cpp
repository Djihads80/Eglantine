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
    if (ParseDailyTag(tag)) {
        return Channel::Daily;
    }
    const auto version = ParseTag(tag);
    if (!version) {
        return std::nullopt;
    }
    return version->rc == 0 ? Channel::Stable : Channel::ReleaseCandidate;
}

std::string_view BuildTag() {
    if (ParseTag(Common::g_build_version)) {
        return Common::g_build_version;
    }
    // A daily build is not a release build: its tag is in the build id, not the version.
    if (ParseDailyTag(Common::g_build_id)) {
        return Common::g_build_id;
    }
    return {};
}

std::optional<Channel> BuildChannel() {
    return ChannelOfTag(BuildTag());
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
    const auto current_channel = ChannelOfTag(current_tag);
    if (!current_channel) {
        return std::nullopt;
    }
    const bool to_daily = channel == Channel::Daily;
    const bool switching = to_daily != (*current_channel == Channel::Daily);
    const auto has_asset = [&](const Common::Net::Release& release) {
        return platform.empty() || !release.GetPlatformAssets(platform, package).empty();
    };

    const Common::Net::Release* selected = nullptr;
    if (to_daily) {
        auto newest = switching ? std::nullopt : ParseDailyTag(current_tag);
        for (const auto& release : releases) {
            const auto date = ParseDailyTag(release.tag);
            if (!date || (newest && *date <= *newest) || !has_asset(release)) {
                continue;
            }
            newest = date;
            selected = &release;
        }
    } else {
        auto newest = switching ? std::nullopt : ParseTag(current_tag);
        for (const auto& release : releases) {
            const auto version = ParseTag(release.tag);
            if (!version || (newest && *version <= *newest)) {
                continue;
            }
            if (channel == Channel::Stable && version->rc != 0) {
                continue;
            }
            if (!has_asset(release)) {
                continue;
            }
            newest = version;
            selected = &release;
        }
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

std::string DownloadPlatform(Settings::UpdateVariant variant, std::string_view build_platform) {
    if (build_platform.empty() || variant == Settings::UpdateVariant::Auto) {
        return std::string{build_platform};
    }
    if (build_platform.ends_with("-pgo")) {
        build_platform.remove_suffix(4);
    }
    std::string platform{build_platform};
    if (variant == Settings::UpdateVariant::Pgo) {
        platform += "-pgo";
    }
    return platform;
}

CheckResult Check(Channel channel, std::string_view platform) {
    const auto current_tag = BuildTag();
    if (current_tag.empty()) {
        return {.failure = Failure::NotReleaseBuild};
    }

    const auto list = Common::Net::GetReleases();
    if (!list.ok) {
        LOG_WARNING(Frontend, "Update check failed with status {}", list.status);
        return {.failure = ClassifyFailure(list.status), .status = list.status};
    }

    auto update = SelectUpdate(list.releases, current_tag, channel, platform,
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
