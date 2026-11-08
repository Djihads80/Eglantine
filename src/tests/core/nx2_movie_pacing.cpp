// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_movie_pacing.h"

TEST_CASE("MKW movie pacing is limited to recent large frames in the current session", "[nx2]") {
    using Service::MkwMovieActivity;
    using namespace std::chrono_literals;
    CHECK(MkwMovieActivity::IsLargeMovie(0x0400C3F00006E000ULL, 2560));
    CHECK_FALSE(MkwMovieActivity::IsLargeMovie(0x0400C3F00006E000ULL, 640));
    CHECK_FALSE(MkwMovieActivity::IsLargeMovie(0x0400123400000000ULL, 2560));

    MkwMovieActivity activity;
    MkwMovieActivity other_session;
    const MkwMovieActivity::Clock::time_point start{1s};
    CHECK_FALSE(activity.Active(start));
    activity.RecordFrame(start);
    CHECK(activity.Active(start));
    CHECK(activity.Active(start + 249ms));
    CHECK_FALSE(activity.Active(start + 250ms));
    CHECK_FALSE(activity.Active(start - 1ms));
    CHECK_FALSE(other_session.Active(start));
    activity.RecordFrame(start + 200ms);
    CHECK(activity.Active(start + 300ms));
    activity.Reset();
    CHECK_FALSE(activity.Active(start + 300ms));
}
