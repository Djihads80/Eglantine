// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/dma_pusher.h"

namespace {

Tegra::CommandList CopySubmission(const std::vector<std::vector<u32>>& entries, u64 generation) {
    Tegra::CommandList list{entries.size()};
    list.snapshot_suspensions = generation;
    for (size_t index = 0; index < entries.size(); ++index) {
        list.command_lists[index].raw = 0;
        list.command_lists[index].size.Assign(entries[index].size());
        list.submitted_offsets.push_back(static_cast<u32>(list.submitted_words.size()));
        for (const u32 word : entries[index]) {
            list.submitted_words.push_back(Tegra::CommandHeader{word});
        }
    }
    return list;
}

std::vector<u32> Words(std::span<const Tegra::CommandHeader> commands) {
    std::vector<u32> words;
    for (const auto command : commands) {
        words.push_back(command.argument);
    }
    return words;
}

TEST_CASE("Resumed command entries use the fresh owned tail after an acquire",
          "[video_core][dma][snapshot]") {
    const u32 header = Tegra::BuildCommandHeader(Tegra::BufferMethods::SemaphoreAddressHigh, 4,
                                                Tegra::SubmissionMode::Increasing).argument;
    std::vector<u32> guest{header, 0, 0x2000, 7, 1, 0x20020100, 11, 22};
    auto list = CopySubmission({guest}, 0);
    constexpr size_t resume = 5; // The acquire consumed its own word before suspending.
    REQUIRE_FALSE(list.SnapshotForEntry(0, resume, 1, false, false));
    REQUIRE_FALSE(list.SnapshotForEntry(0, resume, 2, false, false));

    // The CPU publishes new tail words before releasing the acquire. RefreshSnapshot copies
    // these words and publishes the even resume generation before Step selects its source.
    guest[6] = 33;
    guest[7] = 44;
    list = CopySubmission({guest}, 2);
    const auto selected = list.SnapshotForEntry(0, resume, 2, false, false);
    REQUIRE(selected);
    CHECK(selected->entry.size() == guest.size());
    CHECK(selected->remaining.data() == selected->entry.data() + resume);
    CHECK(Words(selected->remaining) == std::vector<u32>{0x20020100, 33, 44});

    std::vector<u32> consumed;
    for (const auto command : selected->remaining) {
        // The command ring is reused while its previous generation is being consumed.
        std::fill(guest.begin(), guest.end(), 0xdeadbeef);
        consumed.push_back(command.argument);
    }
    CHECK(consumed == std::vector<u32>{0x20020100, 33, 44});
    CHECK(selected->entry.front().argument == header);
}

TEST_CASE("Snapshot slices preserve payload continuation across command entries",
          "[video_core][dma][snapshot]") {
    const u32 header = Tegra::BuildCommandHeader(static_cast<Tegra::BufferMethods>(0x100), 4,
                                                Tegra::SubmissionMode::NonIncreasing).argument;
    const auto list = CopySubmission({{header, 11, 22}, {33, 44}}, 6);
    const auto first = list.SnapshotForEntry(0, 2, 6, false, false);
    const auto second = list.SnapshotForEntry(1, 0, 6, false, false);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(Words(first->entry) == std::vector<u32>{header, 11, 22});
    CHECK(Words(first->remaining) == std::vector<u32>{22});
    CHECK(Words(second->remaining) == std::vector<u32>{33, 44});
    CHECK(second->entry.data() == first->entry.data() + first->entry.size());
    const auto finished = list.SnapshotForEntry(0, 3, 6, false, false);
    REQUIRE(finished);
    CHECK(finished->remaining.empty());
}

TEST_CASE("Command snapshot selection rejects stale GPU-written and invalid entries",
          "[video_core][dma][snapshot]") {
    auto list = CopySubmission({{1, 2, 3, 4}}, 8);
    for (const size_t resume : {size_t{0}, size_t{2}}) {
        CAPTURE(resume);
        CHECK(list.SnapshotForEntry(0, resume, 8, false, false).has_value());
        CHECK_FALSE(list.SnapshotForEntry(0, resume, 8, true, false));
        CHECK_FALSE(list.SnapshotForEntry(0, resume, 8, false, true));
        CHECK_FALSE(list.SnapshotForEntry(0, resume, 10, false, false));
        CHECK_FALSE(list.SnapshotForEntry(0, resume, 9, false, false));
    }
    list.snapshot_suspensions = 9;
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 9, false, false));
    list.snapshot_suspensions = 8;
    CHECK_FALSE(list.SnapshotForEntry(1, 0, 8, false, false));
    CHECK_FALSE(list.SnapshotForEntry(0, 5, 8, false, false));
    list.submitted_words.pop_back();
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 8, false, false));
    list.submitted_offsets[0] = std::numeric_limits<u32>::max();
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 8, false, false));
    list.submitted_offsets.clear();
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 8, false, false));
}

} // namespace


namespace {

TEST_CASE("Immutable diagnostic retains a stable submitted packet across resume epochs",
          "[video_core][dma][snapshot][immutable_snapshot]") {
    const u32 header = Tegra::BuildCommandHeader(Tegra::BufferMethods::SemaphoreAddressHigh, 4,
                                                Tegra::SubmissionMode::Increasing).argument;
    std::vector<u32> first{header, 0x21, 0x13001034}, second{7, 1};
    auto list = CopySubmission({first, second}, 20);
    list.CompleteSubmittedSnapshot(20);
    CHECK(list.HasImmutableSnapshot(true));
    CHECK_FALSE(list.HasImmutableSnapshot(false));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 24, false, false));
    first = {0x90010040, 0x80002040, 0x90000040};
    second = {header, 0x21}; // Later ring reuse would turn a header into an acquire target.
    const auto old_first = list.SnapshotForEntry(0, 0, 24, false, false, true);
    const auto old_second = list.SnapshotForEntry(1, 0, 24, false, false, true);
    REQUIRE(old_first);
    REQUIRE(old_second);
    CHECK(Words(old_first->entry) == std::vector<u32>{header, 0x21, 0x13001034});
    CHECK(Words(old_second->entry) == std::vector<u32>{7, 1});
    const auto resumed = list.SnapshotForEntry(0, 2, 24, false, false, true);
    REQUIRE(resumed);
    CHECK(Words(resumed->remaining) == std::vector<u32>{0x13001034});
}

TEST_CASE("Immutable diagnostic rejects pending unstable or known GPU-written snapshots",
          "[video_core][dma][snapshot][immutable_snapshot]") {
    auto list = CopySubmission({{1, 2, 3}}, 20);
    list.CompleteSubmittedSnapshot(22);
    CHECK_FALSE(list.HasImmutableSnapshot(true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, true));
    list = CopySubmission({{1, 2, 3}}, 21); // Submit during a pending acquire still copies words.
    list.CompleteSubmittedSnapshot(21);
    CHECK_FALSE(list.HasImmutableSnapshot(true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, true));
    list = CopySubmission({{1, 2, 3}}, 20);
    list.CompleteSubmittedSnapshot(20);
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 23, false, false, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 24, true, false, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 24, false, true, true));
    CHECK_FALSE(list.SnapshotForEntry(1, 0, 24, false, false, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 4, 24, false, false, true));
    list.submitted_words.pop_back();
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 24, false, false, true));
}

} // namespace

namespace {
TEST_CASE("Stable-odd diagnostic requires both permissions and a completed resume",
          "[video_core][dma][snapshot][immutable_snapshot]") {
    std::vector<u32> producer{7, 1, 0x1001};
    auto list = CopySubmission({producer}, 21);
    list.CompleteSubmittedSnapshot(21);
    CHECK_FALSE(list.submitted_snapshot_stable);
    CHECK(list.submitted_snapshot_epoch_unchanged);
    CHECK_FALSE(list.HasImmutableSnapshot(false, true));
    CHECK_FALSE(list.HasImmutableSnapshot(true, false));
    CHECK(list.HasImmutableSnapshot(true, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, false, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, true, false));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 21, false, false, true, true));
    const auto view = list.SnapshotForEntry(0, 1, 24, false, false, true, true);
    REQUIRE(view);
    CHECK(Words(view->remaining) == std::vector<u32>{1, 0x1001});
    // Counterexample: a legitimate CPU patch is invisible to epoch/BufferCache guards.
    // The diagnostic deliberately retains the old copy; it does not prove ownership.
    producer[0] = 37;
    const auto retained = list.SnapshotForEntry(0, 0, 24, false, false, true, true);
    REQUIRE(retained);
    CHECK(Words(retained->entry)[0] == 7);
    CHECK(Words(retained->entry)[0] != producer[0]);
}
TEST_CASE("Stable-odd diagnostic still rejects changed epochs GPU writes and invalid ranges",
          "[video_core][dma][snapshot][immutable_snapshot]") {
    auto list = CopySubmission({{1, 2, 3}}, 21);
    list.CompleteSubmittedSnapshot(22);
    CHECK_FALSE(list.submitted_snapshot_epoch_unchanged);
    CHECK_FALSE(list.HasImmutableSnapshot(true, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, true, true));
    list.CompleteSubmittedSnapshot(21);
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, true, false, true, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, true, true, true));
    CHECK_FALSE(list.SnapshotForEntry(1, 0, 22, false, false, true, true));
    CHECK_FALSE(list.SnapshotForEntry(0, 4, 22, false, false, true, true));
    list.submitted_words.pop_back();
    CHECK_FALSE(list.SnapshotForEntry(0, 0, 22, false, false, true, true));
}
} // namespace
