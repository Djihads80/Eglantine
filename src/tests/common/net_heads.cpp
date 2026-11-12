// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/net/net.h"

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
