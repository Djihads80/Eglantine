// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/nx2_command_trace.h"

namespace Trace = VideoCommon::CommandTrace;

TEST_CASE("Command trace window is narrow and inclusive", "[video_core][command_trace]") {
    CHECK_FALSE(Trace::InWindow(3299));
    CHECK(Trace::InWindow(3300));
    CHECK(Trace::InWindow(3500));
    CHECK_FALSE(Trace::InWindow(3501));
}
TEST_CASE("Command trace history retains chronological bounded records by value", "[video_core][command_trace]") {
    Trace::History<2> history;
    Trace::Event event;
    for (u64 i = 1; i <= 3; ++i) { event.origin.submission = i; history.Append(event); }
    event.origin.submission = 9;
    const auto saved = history.Snapshot();
    REQUIRE(saved.size() == 2);
    CHECK(saved[0].origin.submission == 2);
    CHECK(saved[1].origin.submission == 3);
    CHECK(history.Total() == 3);
}
TEST_CASE("Command refresh comparison records the first owned-word difference", "[video_core][command_trace]") {
    const std::array<u32, 4> before{1, 2, 3, 4}, after{1, 8, 9, 4};
    auto difference = Trace::FirstDifference<u32>(before, after);
    CHECK(difference.offset == 1);
    CHECK(difference.before == 2);
    CHECK(difference.after == 8);
    CHECK(Trace::FirstDifference<u32>(before, before).offset == std::numeric_limits<u64>::max());
    CHECK(Trace::Fingerprint<u32>(before) != Trace::Fingerprint<u32>(after));
}
TEST_CASE("Command consumption distinguishes actual scalar divergence and opaque live bulk", "[video_core][command_trace]") {
    const std::array<u32, 4> baseline{1, 2, 3, 4};
    Trace::Consumption<u32> consumption{baseline};
    consumption.Observe(0, 1);
    consumption.Opaque(2); // Must not peek into a direct guest span to complete the fingerprint.
    consumption.Observe(3, 8);
    const auto event = consumption.Finish({.submission = 12, .offset = 0}, 4);
    CHECK(event.origin.submission == 12);
    CHECK(event.b == 4);
    CHECK(event.c == 3);
    CHECK(event.d == 2);
    CHECK(event.words[0] == 4);
    CHECK(event.words[1] == 8);
    CHECK(event.words[2] == 2);
}
TEST_CASE("Command release origins are copied and scoped across nested callbacks", "[video_core][command_trace]") {
    Trace::current_origin = {.submission = 10, .present = 3300};
    Trace::Origin retained;
    {
        Trace::ScopedOrigin scope{{.submission = 20, .present = 3400}};
        retained = Trace::CurrentOrigin(7);
        {
            Trace::ScopedOrigin nested{{.submission = 30, .present = 3401}};
            CHECK(Trace::current_origin.submission == 30);
        }
        CHECK(Trace::current_origin.submission == 20);
    }
    CHECK(Trace::current_origin.submission == 10);
    CHECK(retained.submission == 20);
    CHECK(retained.asid == 7);
    Trace::current_origin = {};
}

TEST_CASE("Small changed command entries retain complete owned generations", "[video_core][command_trace]") {
    std::array<u32, 3> before{0x20010004, 0x21, 0x218014}, after{0x20010004, 0x21, 0x118014};
    auto result = Trace::CaptureSmallEntry<u32>(before, after,
        {.origin = {.submission = 4, .asid = 2, .entry = 0x1200, .generation = 6, .channel = 1},
         .new_generation = 8, .entry_index = 3, .first_difference = 2,
         .refresh_parser = {.method = 0x45, .count = 7, .subchannel = 2, .flags = 1}});
    REQUIRE(result);
    before.fill(0); after.fill(0);
    CHECK(result->entry_words == 3);
    CHECK(result->before[2] == 0x218014);
    CHECK(result->after[2] == 0x118014);
    CHECK(result->before[3] == 0);
    CHECK(result->refresh_parser.count == 7);
    const std::array<u32, 17> large{};
    CHECK_FALSE(Trace::CaptureSmallEntry<u32>(large, large, {}));
    CHECK_FALSE(Trace::CaptureSmallEntry<u32>({}, {}, {}));
    CHECK_FALSE(Trace::CaptureSmallEntry<u32>(before, std::span<const u32>{after}.first(2), {}));
}
TEST_CASE("Small entry parse attribution respects identity and active generation", "[video_core][command_trace]") {
    Trace::SmallEntryHistory history;
    const Trace::Origin origin{.submission = 4, .asid = 2, .entry = 0x1200, .offset = 2,
                               .generation = 8, .channel = 1, .source = Trace::Source::Refreshed};
    history.Append({.origin = origin, .ns = 10, .new_generation = 8});
    auto wrong = origin; wrong.generation = 6;
    history.ObserveParse(wrong, {}, 20, 3);
    wrong = origin; wrong.asid = 9;
    history.ObserveParse(wrong, {}, 20, 3);
    wrong = origin; wrong.submission = 5;
    history.ObserveParse(wrong, {}, 20, 3);
    CHECK_FALSE(history.Entries()[0].parsed);
    history.ObserveParse(origin, {.method = 0x45, .count = 2, .subchannel = 3, .flags = 3}, 20, 3);
    REQUIRE(history.Entries()[0].parsed);
    CHECK(history.Entries()[0].parse_parser.method == 0x45);
    CHECK(history.Entries()[0].parse_parser.count == 2);
    CHECK(history.Entries()[0].available_parse_words == 3);
    CHECK(history.Entries()[0].parse_offset == 2);
    CHECK(history.Entries()[0].parse_source == Trace::Source::Refreshed);
    history.ObserveParse(origin, {.method = 0x88}, 30, 1);
    CHECK(history.Entries()[0].parse_parser.method == 0x45);
    for (size_t i = 0; i < 150; ++i) {
        history.Append({});
    }
    CHECK(history.Entries().size() == 128);
}
