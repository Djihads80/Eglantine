// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include "common/net/net.h"
#include "common/scm_rev.h"

namespace {

using Common::Net::BranchNamesFromJson;
using Common::Net::RemoteHead;

constexpr std::string_view kSha = "0123456789abcdef0123456789abcdef01234567";

std::string BranchJson(const std::string& sha) {
    return fmt::format(R"({{"name":"main","commit":{{"sha":"{}"}}}})", sha);
}

std::string PullJson(const std::string& sha, const std::string& extra) {
    return fmt::format(R"({{"title":"Fix","head":{{"sha":"{}"}}{}}})", sha, extra);
}

} // namespace

TEST_CASE("Net: branch head from JSON", "[tester]") {
    const auto head = RemoteHead::FromBranchJson(BranchJson(std::string{kSha}));
    REQUIRE(head);
    CHECK(head->sha == kSha);
}

TEST_CASE("Net: branch head sha is lowercased", "[tester]") {
    const auto head = RemoteHead::FromBranchJson(BranchJson("0123456789ABCDEF0123456789ABCDEF01234567"));
    REQUIRE(head);
    CHECK(head->sha == kSha);
}

TEST_CASE("Net: branch head rejects a bad sha", "[tester]") {
    CHECK_FALSE(RemoteHead::FromBranchJson(R"({"commit":{}})").has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(R"({"commit":"abc"})").has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(R"({"commit":{"sha":123}})").has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(BranchJson("0123456789abcdef")).has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(BranchJson(std::string(39, 'a'))).has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(BranchJson(std::string(41, 'a'))).has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson(BranchJson(std::string(40, 'g'))).has_value());
}

TEST_CASE("Net: branch head rejects malformed JSON", "[tester]") {
    CHECK_FALSE(RemoteHead::FromBranchJson("").has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson("{").has_value());
    CHECK_FALSE(RemoteHead::FromBranchJson("not json").has_value());
}

TEST_CASE("Net: pull head state", "[tester]") {
    SECTION("open") {
        const auto head = RemoteHead::FromPullJson(
            PullJson(std::string{kSha}, R"(,"state":"open","merged":false,"user":{"login":"octo"})"));
        REQUIRE(head);
        CHECK(head->sha == kSha);
        CHECK(head->title == "Fix");
        CHECK(head->author == "octo");
        CHECK(head->open);
        CHECK_FALSE(head->merged);
    }
    SECTION("closed and unmerged") {
        const auto head = RemoteHead::FromPullJson(
            PullJson(std::string{kSha}, R"(,"state":"closed","merged":false,"user":{"login":"octo"})"));
        REQUIRE(head);
        CHECK_FALSE(head->open);
        CHECK_FALSE(head->merged);
    }
    SECTION("merged") {
        const auto head = RemoteHead::FromPullJson(
            PullJson(std::string{kSha}, R"(,"state":"closed","merged":true,"user":{"login":"octo"})"));
        REQUIRE(head);
        CHECK_FALSE(head->open);
        CHECK(head->merged);
    }
    SECTION("merged null is not merged") {
        const auto head = RemoteHead::FromPullJson(
            PullJson(std::string{kSha}, R"(,"state":"open","merged":null,"user":{"login":"octo"})"));
        REQUIRE(head);
        CHECK(head->open);
        CHECK_FALSE(head->merged);
    }
}

TEST_CASE("Net: pull head without a user", "[tester]") {
    const auto head = RemoteHead::FromPullJson(PullJson(std::string{kSha}, R"(,"state":"open")"));
    REQUIRE(head);
    CHECK(head->sha == kSha);
    CHECK(head->author.empty());
}

TEST_CASE("Net: pull head needs a valid sha", "[tester]") {
    CHECK_FALSE(RemoteHead::FromPullJson(PullJson("abc", R"(,"state":"open")")).has_value());
    CHECK_FALSE(RemoteHead::FromPullJson(R"({"title":"Fix"})").has_value());
    CHECK_FALSE(RemoteHead::FromPullJson("[").has_value());
}

TEST_CASE("Net: branch names from JSON", "[tester]") {
    const auto names = BranchNamesFromJson(
        R"([{"name":"main","commit":{"sha":"0123"}},{"commit":{"sha":"4567"}},{"name":"updater/tagged-channels"}])");
    CHECK(names == std::vector<std::string>{"main", "updater/tagged-channels"});
    CHECK(BranchNamesFromJson("[]").empty());
    CHECK(BranchNamesFromJson(R"([{"name":)").empty());
    CHECK(BranchNamesFromJson("").empty());
}

TEST_CASE("Net: recent branches from the activity JSON", "[tester]") {
    using Common::Net::RecentBranchesFromActivityJson;
    const std::string json = R"([
        {"ref":"refs/heads/nx2-experimental","activity_type":"push"},
        {"ref":"refs/heads/gone","activity_type":"branch_deletion"},
        {"ref":"refs/tags/daily-20261112","activity_type":"branch_deletion"},
        {"ref":"refs/heads/nx2","activity_type":"pr_merge"},
        {"ref":"refs/heads/nx2-experimental","activity_type":"push"},
        {"ref":"refs/heads/gone","activity_type":"push"},
        {"activity_type":"push"},
        {"ref":"refs/heads/fix/lag","activity_type":"branch_creation"}
    ])";
    CHECK(RecentBranchesFromActivityJson(json, 10) ==
          std::vector<std::string>{"nx2-experimental", "nx2", "fix/lag"});
    CHECK(RecentBranchesFromActivityJson(json, 2) ==
          std::vector<std::string>{"nx2-experimental", "nx2"});
    CHECK(RecentBranchesFromActivityJson("[]", 10).empty());
    CHECK(RecentBranchesFromActivityJson(R"({"message":"Not Found"})", 10).empty());
    CHECK(RecentBranchesFromActivityJson("", 10).empty());
}

namespace {

constexpr std::string_view kOtherSha = "fedcba9876543210fedcba9876543210fedcba98";

// A release for `target` (a commit or anything else) with the given asset files.
Common::Net::Release MakeCommitRelease(const std::string& tag, const std::string& target,
                                       const std::string& published,
                                       const std::vector<std::string>& files) {
    const std::string repo = Common::g_build_auto_update_repo;
    auto assets = nlohmann::json::array();
    for (const auto& file : files) {
        assets.push_back({{"browser_download_url",
                           fmt::format("https://github.com/{}/releases/download/{}/{}", repo, tag,
                                       file)},
                          {"size", 10}});
    }
    const nlohmann::json json = {{"tag_name", tag},
                                 {"target_commitish", target},
                                 {"published_at", published},
                                 {"assets", assets}};
    auto release = Common::Net::Release::FromJson(json, "https://github.com", repo);
    REQUIRE(release.has_value());
    return *release;
}

} // namespace

TEST_CASE("Net: release target commit", "[tester]") {
    const auto sha = std::string{kSha};
    CHECK(MakeCommitRelease("a", sha, "2026-01-01T00:00:00Z", {}).target_commit == kSha);
    CHECK(MakeCommitRelease("a", "0123456789ABCDEF0123456789ABCDEF01234567",
                            "2026-01-01T00:00:00Z", {})
              .target_commit == kSha);
    CHECK(MakeCommitRelease("a", "nx2", "2026-01-01T00:00:00Z", {}).target_commit.empty());
    CHECK(MakeCommitRelease("a", sha.substr(0, 39), "2026-01-01T00:00:00Z", {})
              .target_commit.empty());
    CHECK(MakeCommitRelease("a", std::string(40, 'g'), "2026-01-01T00:00:00Z", {})
              .target_commit.empty());

    const nlohmann::json no_target = {{"tag_name", "a"}, {"assets", nlohmann::json::array()}};
    const auto release = Common::Net::Release::FromJson(no_target, "https://github.com", "o/r");
    REQUIRE(release);
    CHECK(release->target_commit.empty());
}

TEST_CASE("Net: FindReleaseForCommit", "[tester]") {
    using Common::Net::FindReleaseForCommit;
    using Common::Net::LinuxPackage;
    const auto sha = std::string{kSha};
    const std::vector<std::string> windows = {"eglantine-windows-x86_64.zip"};
    const std::vector<std::string> linux_files = {"eglantine-linux-x86_64.AppImage",
                                                  "eglantine-linux-x86_64.tar.gz"};

    const std::vector<Common::Net::Release> releases = {
        MakeCommitRelease("branch-dev-1", sha, "2026-01-01T00:00:00Z", windows),
        MakeCommitRelease("branch-dev-2", sha, "2026-03-01T00:00:00Z", windows),
        MakeCommitRelease("branch-dev-0", sha, "2026-02-01T00:00:00Z", windows),
        // Targets a branch name, not a commit.
        MakeCommitRelease("daily", "dev", "2026-06-01T00:00:00Z", windows),
        // Right commit, wrong platform.
        MakeCommitRelease("linux-only", sha, "2026-07-01T00:00:00Z", linux_files),
        MakeCommitRelease("other", std::string{kOtherSha}, "2026-08-01T00:00:00Z", windows),
    };

    // Several matches: the most recently published one.
    const auto found = FindReleaseForCommit(releases, kSha, "windows-x86_64");
    REQUIRE(found);
    CHECK(found->tag == "branch-dev-2");

    const auto linux_found =
        FindReleaseForCommit(releases, kSha, "linux-x86_64", LinuxPackage::AppImage);
    REQUIRE(linux_found);
    CHECK(linux_found->tag == "linux-only");

    const auto other = FindReleaseForCommit(releases, kOtherSha, "windows-x86_64");
    REQUIRE(other);
    CHECK(other->tag == "other");
    CHECK_FALSE(FindReleaseForCommit(releases, kOtherSha, "linux-x86_64").has_value());
    CHECK_FALSE(FindReleaseForCommit(releases, kSha, "macos-arm64").has_value());
    CHECK_FALSE(FindReleaseForCommit(releases, kSha, "").has_value());
    CHECK_FALSE(
        FindReleaseForCommit(releases, std::string(40, 'b'), "windows-x86_64").has_value());
    CHECK_FALSE(FindReleaseForCommit(releases, "dev", "windows-x86_64").has_value());
    CHECK_FALSE(FindReleaseForCommit(releases, "", "windows-x86_64").has_value());
    CHECK_FALSE(FindReleaseForCommit({}, kSha, "windows-x86_64").has_value());
}
