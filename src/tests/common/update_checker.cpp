// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace {

using namespace UpdateChecker;

constexpr std::string_view kPlatform = "windows-x86_64";

// A release shaped like the GitHub API; each file name becomes an asset of the release.
Common::Net::Release MakeRelease(const std::string& tag,
                                 const std::vector<std::string>& files = {
                                     "eglantine-windows-x86_64.zip"}) {
    const std::string repo = Common::g_build_auto_update_repo;
    auto assets = nlohmann::json::array();
    for (const auto& file : files) {
        assets.push_back({
            {"browser_download_url",
             fmt::format("https://github.com/{}/releases/download/{}/{}", repo, tag, file)},
            {"size", 1234},
            {"digest", "sha256:" + std::string(64, 'a')},
        });
    }
    const nlohmann::json json = {
        {"tag_name", tag},
        {"name", tag},
        {"prerelease", tag.find("-rc") != std::string::npos},
        {"assets", assets},
    };
    auto release = Common::Net::Release::FromJson(json, "https://github.com", repo);
    REQUIRE(release.has_value());
    return *release;
}

std::vector<Common::Net::Release> MakeReleases(const std::vector<std::string>& tags) {
    std::vector<Common::Net::Release> releases;
    for (const auto& tag : tags) {
        releases.push_back(MakeRelease(tag));
    }
    return releases;
}

std::string Select(const std::vector<Common::Net::Release>& releases, std::string_view current,
                   Channel channel, std::string_view platform = kPlatform) {
    const auto update = SelectUpdate(releases, current, channel, platform);
    return update ? update->tag : std::string{};
}

} // namespace

TEST_CASE("UpdateChecker: tag parsing", "[updater]") {
    for (const char* tag : {"v0.1.0", "v1.20.300", "v0.1.0-rc1", "v0.1.0-rc10"}) {
        INFO(tag);
        CHECK(ParseTag(tag).has_value());
    }
    for (const char* tag :
         {"", "v", "daily-20261109", "branch-foo-abc123", "build-219", "9.0.2-av1-mingw-1", "v0.1",
          "v0.1.0-", "v0.1.0-rc", "v0.1.0-rc0", "v0.1.0-beta1", "v0.1.0-rc1x", "v0.1.0.1", "0.1.0",
          "V0.1.0", "v0.1.0 ", "v01.2.3", "v1.02.3", "v1.2.03", "v0.1.0-rc01",
          "v99999999999999999999.0.0", "113e7561cd-updater-tagged-channels"}) {
        INFO(tag);
        CHECK_FALSE(ParseTag(tag).has_value());
    }

    const auto rc = ParseTag("v1.2.3-rc4");
    REQUIRE(rc);
    CHECK(rc->core == std::array<u64, 3>{1, 2, 3});
    CHECK(rc->rc == 4);
    CHECK(ParseTag("v1.2.3")->rc == 0);
}

TEST_CASE("UpdateChecker: ordering", "[updater]") {
    const char* ascending[] = {"v0.1.0-rc3", "v0.1.0-rc10", "v0.1.0",      "v0.1.1-rc1",
                               "v0.1.1",     "v0.2.0-rc1",  "v0.10.0-rc1", "v1.0.0"};
    for (std::size_t i = 0; i < std::size(ascending); ++i) {
        for (std::size_t j = 0; j < std::size(ascending); ++j) {
            INFO(ascending[i] << " vs " << ascending[j]);
            CHECK((*ParseTag(ascending[i]) < *ParseTag(ascending[j])) == (i < j));
            CHECK((*ParseTag(ascending[i]) == *ParseTag(ascending[j])) == (i == j));
        }
    }
}

TEST_CASE("UpdateChecker: channel of a tag", "[updater]") {
    CHECK(ChannelOfTag("v0.1.0") == Channel::Stable);
    CHECK(ChannelOfTag("v0.1.0-rc2") == Channel::ReleaseCandidate);
    CHECK(ChannelOfTag("daily-20261109") == Channel::Daily);
    CHECK_FALSE(ChannelOfTag("113e7561cd-branch").has_value());
}

TEST_CASE("UpdateChecker: channel selection", "[updater]") {
    const auto releases = MakeReleases({"v0.1.0-rc3", "v0.1.0", "v0.1.1-rc1", "v0.2.0-rc1"});

    SECTION("Stable ignores release candidates") {
        CHECK(Select(releases, "v0.0.9", Channel::Stable) == "v0.1.0");
        CHECK(Select(releases, "v0.1.0", Channel::Stable) == "");
    }
    SECTION("RC takes the newest of rc and stable") {
        CHECK(Select(releases, "v0.0.9", Channel::ReleaseCandidate) == "v0.2.0-rc1");
        const auto older = MakeReleases({"v0.1.0-rc3", "v0.1.0", "v0.1.1-rc1"});
        CHECK(Select(older, "v0.0.9", Channel::ReleaseCandidate) == "v0.1.1-rc1");
        const auto stable_wins = MakeReleases({"v0.1.0-rc3", "v0.1.0"});
        CHECK(Select(stable_wins, "v0.0.9", Channel::ReleaseCandidate) == "v0.1.0");
    }
    SECTION("an rc build on the Stable channel is offered the next stable, not a later rc") {
        const auto list = MakeReleases({"v0.1.0-rc3", "v0.1.0-rc4"});
        CHECK(Select(list, "v0.1.0-rc3", Channel::Stable) == "");
        CHECK(Select(releases, "v0.1.0-rc3", Channel::Stable) == "v0.1.0");
        CHECK(Select(releases, "v0.1.0-rc3", Channel::ReleaseCandidate) == "v0.2.0-rc1");
    }
    SECTION("a stable build on the RC channel is offered the next version's rc") {
        CHECK(Select(releases, "v0.1.0", Channel::ReleaseCandidate) == "v0.2.0-rc1");
        const auto list = MakeReleases({"v0.1.0", "v0.1.1-rc1"});
        CHECK(Select(list, "v0.1.0", Channel::ReleaseCandidate) == "v0.1.1-rc1");
    }
    SECTION("nothing is offered when already newest") {
        CHECK(Select(releases, "v0.2.0-rc1", Channel::ReleaseCandidate) == "");
        CHECK(Select(releases, "v0.3.0", Channel::ReleaseCandidate) == "");
        CHECK(Select({}, "v0.1.0", Channel::ReleaseCandidate) == "");
    }
    SECTION("list order does not matter") {
        const auto shuffled = MakeReleases({"v0.2.0-rc1", "v0.1.0", "v0.1.1-rc1", "v0.1.0-rc3"});
        CHECK(Select(shuffled, "v0.0.1", Channel::ReleaseCandidate) == "v0.2.0-rc1");
    }
}

TEST_CASE("UpdateChecker: untagged releases are ignored", "[updater]") {
    const auto releases = MakeReleases({"daily-20261109", "branch-foo-abc123", "build-219",
                                        "9.0.2-av1-mingw-1", "v0.1", "v0.1.0-beta1", "v0.1.1"});
    CHECK(Select(releases, "v0.1.0", Channel::ReleaseCandidate) == "v0.1.1");
    CHECK(Select(releases, "v0.1.1", Channel::ReleaseCandidate) == "");

    const auto only_untagged = MakeReleases({"daily-20261109", "branch-foo-abc123"});
    CHECK(Select(only_untagged, "v0.1.0", Channel::ReleaseCandidate) == "");
}

TEST_CASE("UpdateChecker: a non-release build is offered nothing", "[updater]") {
    const auto releases = MakeReleases({"v0.1.0", "v0.2.0-rc1"});
    for (const char* current : {"", "113e7561cd-branch", "branch-foo-abc123", "v0.1"}) {
        INFO(current);
        CHECK(Select(releases, current, Channel::ReleaseCandidate) == "");
        CHECK(Select(releases, current, Channel::Stable) == "");
        CHECK(Select(MakeReleases({"daily-20261109"}), current, Channel::Daily) == "");
    }
}

TEST_CASE("UpdateChecker: daily tag parsing", "[updater]") {
    CHECK(ParseDailyTag("daily-20261109") == 20261109u);
    CHECK(ParseDailyTag("daily-20270101") == 20270101u);
    for (const char* tag : {"", "daily-", "daily-2026110", "daily-202611090", "daily-2026110x",
                            "daily-20261309", "daily-20261100", "daily-20260032", "Daily-20261109",
                            "daily-20261109 ", "v0.1.0", "branch-daily-20261109"}) {
        INFO(tag);
        CHECK_FALSE(ParseDailyTag(tag).has_value());
    }
}

TEST_CASE("UpdateChecker: daily channel", "[updater]") {
    const auto releases = MakeReleases(
        {"daily-20261112", "v0.1.0", "daily-20261109", "v0.2.0-rc1", "daily-20261110"});

    SECTION("the newest later daily build is offered") {
        CHECK(Select(releases, "daily-20261105", Channel::Daily) == "daily-20261112");
        CHECK(Select(releases, "daily-20261110", Channel::Daily) == "daily-20261112");
    }
    SECTION("nothing is offered when already newest") {
        CHECK(Select(releases, "daily-20261112", Channel::Daily) == "");
        CHECK(Select(releases, "daily-20261201", Channel::Daily) == "");
    }
    SECTION("tagged releases are not daily builds") {
        const auto tagged = MakeReleases({"v0.1.0", "v0.2.0-rc1"});
        CHECK(Select(tagged, "daily-20261105", Channel::Daily) == "");
    }
    SECTION("a daily build without the platform asset is skipped") {
        const std::vector<Common::Net::Release> list = {
            MakeRelease("daily-20261110"),
            MakeRelease("daily-20261111", {"eglantine-linux-x86_64.AppImage"}),
        };
        CHECK(Select(list, "daily-20261109", Channel::Daily) == "daily-20261110");
        CHECK(Select(list, "daily-20261109", Channel::Daily, "linux-x86_64") == "daily-20261111");
    }
}

TEST_CASE("UpdateChecker: switching between daily and tagged builds", "[updater]") {
    const auto releases = MakeReleases(
        {"daily-20261112", "v0.1.0", "daily-20261109", "v0.2.0-rc1", "v0.0.9"});

    SECTION("a tagged build on the Daily channel is offered the newest daily build") {
        CHECK(Select(releases, "v0.1.0", Channel::Daily) == "daily-20261112");
        CHECK(Select(releases, "v9.0.0", Channel::Daily) == "daily-20261112");
        CHECK(Select(releases, "v0.2.0-rc1", Channel::Daily) == "daily-20261112");
    }
    SECTION("a daily build on a tagged channel is offered that channel's newest release") {
        CHECK(Select(releases, "daily-20261112", Channel::Stable) == "v0.1.0");
        CHECK(Select(releases, "daily-20261201", Channel::Stable) == "v0.1.0");
        CHECK(Select(releases, "daily-20261112", Channel::ReleaseCandidate) == "v0.2.0-rc1");
    }
    SECTION("nothing is offered when the channel has no release") {
        CHECK(Select(MakeReleases({"v0.1.0"}), "v0.1.0", Channel::Daily) == "");
        CHECK(Select(MakeReleases({"daily-20261109"}), "daily-20261105", Channel::Stable) == "");
    }
}

TEST_CASE("UpdateChecker: platform filtering", "[updater]") {
    SECTION("a release without the platform asset is skipped for the next best") {
        const std::vector<Common::Net::Release> releases = {
            MakeRelease("v0.1.1"),
            MakeRelease("v0.1.2", {"eglantine-linux-x86_64.AppImage"}),
            MakeRelease("v0.1.3", {}),
        };
        CHECK(Select(releases, "v0.1.0", Channel::Stable) == "v0.1.1");
        CHECK(Select(releases, "v0.1.0", Channel::Stable, "linux-x86_64") == "v0.1.2");
        CHECK(Select(releases, "v0.1.1", Channel::Stable) == "");
    }
    SECTION("a pgo platform falls back to the standard asset") {
        const std::vector<Common::Net::Release> releases = {
            MakeRelease("v0.1.1", {"eglantine-windows-x86_64-pgo.zip"}),
            MakeRelease("v0.1.2", {"eglantine-windows-x86_64.zip"}),
        };
        CHECK(Select(releases, "v0.1.0", Channel::Stable, "windows-x86_64-pgo") == "v0.1.2");
        const std::vector<Common::Net::Release> pgo_only = {releases[0]};
        CHECK(Select(pgo_only, "v0.1.0", Channel::Stable, "windows-x86_64-pgo") == "v0.1.1");
        CHECK(Select(pgo_only, "v0.1.0", Channel::Stable, "windows-x86_64") == "");
    }
    SECTION("an empty platform does not filter") {
        const std::vector<Common::Net::Release> releases = {MakeRelease("v0.1.1", {})};
        CHECK(Select(releases, "v0.1.0", Channel::Stable, "") == "v0.1.1");
        CHECK(Select(releases, "v0.1.0", Channel::Stable) == "");
    }
}

TEST_CASE("UpdateChecker: failure classification", "[updater]") {
    CHECK(ClassifyFailure(0) == Failure::Network);
    CHECK(ClassifyFailure(200) == Failure::Network);
    CHECK(ClassifyFailure(403) == Failure::RateLimited);
    CHECK(ClassifyFailure(429) == Failure::RateLimited);
    CHECK(ClassifyFailure(400) == Failure::Http);
    CHECK(ClassifyFailure(404) == Failure::Http);
    CHECK(ClassifyFailure(500) == Failure::Http);
    CHECK(ClassifyFailure(503) == Failure::Http);
}
