// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <functional>
#include <utility>

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

bool IsChannelSwitch(std::string_view current_tag, std::string_view tag) {
    return ParseDailyTag(current_tag).has_value() != ParseDailyTag(tag).has_value();
}

std::vector<std::string> RankUpdates(std::span<const Common::Net::Tag> tags,
                                     std::string_view current_tag, std::string_view current_commit,
                                     Channel channel) {
    const auto current_channel = ChannelOfTag(current_tag);
    if (!current_channel) {
        return {};
    }
    const bool to_daily = channel == Channel::Daily;
    const bool switching = to_daily != (*current_channel == Channel::Daily);

    std::vector<std::string> ranked;
    if (to_daily) {
        const auto current_date = switching ? std::nullopt : ParseDailyTag(current_tag);
        std::vector<std::pair<u32, std::string>> found;
        for (const auto& tag : tags) {
            const auto date = ParseDailyTag(tag.name);
            if (!date) {
                continue;
            }
            if (current_date) {
                // An unknown commit on either side is not taken for a rebuild.
                const bool rebuilt =
                    !tag.commit.empty() && !current_commit.empty() && tag.commit != current_commit;
                if (*date < *current_date || (*date == *current_date && !rebuilt)) {
                    continue;
                }
            }
            found.emplace_back(*date, tag.name);
        }
        std::ranges::stable_sort(found, std::greater{}, &std::pair<u32, std::string>::first);
        for (auto& [date, name] : found) {
            ranked.push_back(std::move(name));
        }
    } else {
        const auto current_version = switching ? std::nullopt : ParseTag(current_tag);
        std::vector<std::pair<Version, std::string>> found;
        for (const auto& tag : tags) {
            const auto version = ParseTag(tag.name);
            if (!version || (current_version && *version <= *current_version)) {
                continue;
            }
            if (channel == Channel::Stable && version->rc != 0) {
                continue;
            }
            found.emplace_back(*version, tag.name);
        }
        std::ranges::stable_sort(found, std::greater{}, &std::pair<Version, std::string>::first);
        for (auto& [version, name] : found) {
            ranked.push_back(std::move(name));
        }
    }
    return ranked;
}

namespace {

bool HasAsset(const Common::Net::Release& release, std::string_view platform,
              Common::Net::LinuxPackage package) {
    return platform.empty() || !release.GetPlatformAssets(platform, package).empty();
}

} // namespace

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view current_tag,
                                                 std::string_view current_commit, Channel channel,
                                                 std::string_view platform,
                                                 Common::Net::LinuxPackage package) {
    std::vector<Common::Net::Tag> tags;
    for (const auto& release : releases) {
        tags.push_back({.name = release.tag, .commit = release.commit});
    }
    for (const auto& name : RankUpdates(tags, current_tag, current_commit, channel)) {
        const auto release = std::ranges::find(releases, name, &Common::Net::Release::tag);
        if (HasAsset(*release, platform, package)) {
            return *release;
        }
    }
    return std::nullopt;
}

CheckResult Check(Channel channel) {
    const auto current_tag = BuildTag();
    if (current_tag.empty()) {
        return {.failure = Failure::NotReleaseBuild};
    }

    const auto tags = Common::Net::GetTags(channel == Channel::Daily ? "daily-" : "v");
    if (!tags.ok) {
        LOG_WARNING(Frontend, "Update check failed with status {}", tags.status);
        return {.failure = ClassifyFailure(tags.status), .status = tags.status};
    }

    // Each candidate costs a request against GitHub's unauthenticated rate limit, so only the few
    // best are tried; one is skipped when its release is gone or lacks this platform's asset.
    constexpr std::size_t max_candidates = 3;
    const auto candidates = RankUpdates(tags.tags, current_tag, Common::g_scm_rev, channel);
    for (std::size_t i = 0; i < std::min(candidates.size(), max_candidates); ++i) {
        const auto result = Common::Net::GetRelease(candidates[i]);
        if (!result.ok) {
            if (result.status == 404) {
                continue;
            }
            LOG_WARNING(Frontend, "Update check failed with status {}", result.status);
            return {.failure = ClassifyFailure(result.status), .status = result.status};
        }
        if (result.release && HasAsset(*result.release, Common::g_build_release_platform,
                                       Common::Net::CurrentLinuxPackage())) {
            LOG_INFO(Frontend, "Received update {}", result.release->title);
            return {.update = result.release};
        }
    }
    return {};
}

std::optional<Common::Net::Release> GetUpdate() {
    const auto channel = BuildChannel();
    return channel ? Check(*channel).update : std::nullopt;
}

} // namespace UpdateChecker
