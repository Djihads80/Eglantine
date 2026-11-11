// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/split_write_tracker.h"

namespace {

using VideoCommon::SplitWriteTracker;
using Effect = SplitWriteTracker::Effect;

// Every entry the front queued, kept whole: what a read must wait for, by definition.
struct Model {
    struct Entry {
        u64 position;
        Effect effect;
        u64 begin;
        u64 end;
    };
    std::vector<Entry> entries;

    // The last position the back must pass: unlocalized writes and writes to [begin, end).
    u64 Target(u64 begin, u64 end, u64 consumed) const {
        u64 target = 0;
        for (const Entry& entry : entries) {
            if (entry.position <= consumed) {
                continue;
            }
            const bool needed = entry.effect == Effect::Unknown ||
                                entry.effect == Effect::Gpu ||
                                (entry.begin < end && begin < entry.end);
            if (needed) {
                target = (std::max)(target, entry.position);
            }
        }
        return target;
    }
};

} // Anonymous namespace

TEST_CASE("Split write tracker adds, overlaps and retires writes", "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    tracker.Write(0x1000, 0x2000, 10);
    tracker.Write(0x8000, 0x9000, 20);
    CHECK(tracker.LastWriteTo(0x1800, 0x1804) == 10);
    CHECK(tracker.LastWriteTo(0x8000, 0x8001) == 20);
    CHECK(tracker.LastWriteTo(0x2000, 0x8000) == 0);
    CHECK(tracker.LastWriteTo(0x0, 0x10000) == 20);
    // A write next to the last one joins it and retires with it.
    tracker.Write(0x9000, 0xa000, 30);
    CHECK(tracker.LastWriteTo(0x8000, 0x8004) == 30);
    tracker.Retire(10);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000) == 0);
    CHECK(tracker.LastWriteTo(0x9800, 0x9804) == 30);
    tracker.Retire(29);
    CHECK(tracker.LastWriteTo(0x8000, 0xa000) == 30);
    tracker.Retire(30);
    CHECK(tracker.LastWriteTo(0x0, ~u64{0}) == 0);
}

TEST_CASE("Split write tracker waits for every unlocalized writer",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    tracker.Entry(Effect::Gpu, 5);
    tracker.Entry(Effect::None, 6);
    tracker.Entry(Effect::Gpu, 9);
    CHECK(tracker.ReadTarget(0x1000, 0x1004) == 9);
    tracker.Entry(Effect::None, 12);
    CHECK(tracker.ReadTarget(0x1000, 0x1004) == 9);
    tracker.Entry(Effect::Unknown, 14);
    CHECK(tracker.ReadTarget(0x1000, 0x1004) == 14);
}

TEST_CASE("Split write tracker keeps the kind of constant buffer writes",
          "[video_core][split_writes]") {
    using Kind = SplitWriteTracker::WriteKind;
    SplitWriteTracker tracker;
    tracker.Write(0x1000, 0x2000, 3, Kind::ConstantBuffer);
    tracker.Write(0x1000, 0x2000, 4, Kind::ConstantBuffer);
    CHECK(tracker.LastWriteTo(0x1000, 0x1004) == 4);
    CHECK(tracker.LastWriteTo(0x1000, 0x1004, Kind::Other) == 0);
    // A merged range that mixes kinds is Other.
    tracker.Write(0x2000, 0x3000, 5);
    CHECK(tracker.LastWriteTo(0x1000, 0x1004, Kind::Other) == 5);
    tracker.Write(0x8000, 0x9000, 6, Kind::ConstantBuffer);
    CHECK(tracker.LastWriteTo(0x8000, 0x8004, Kind::Other) == 0);
    CHECK(tracker.LastWriteTo(0x8000, 0x8004, Kind::ConstantBuffer) == 6);
}

TEST_CASE("Split write tracker skips only proven draws for writer reads",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    CHECK(SplitWriteTracker::Combine(Effect::Draw, Effect::None) == Effect::Draw);
    CHECK(SplitWriteTracker::Combine(Effect::Draw, Effect::Gpu) == Effect::Gpu);
    CHECK(SplitWriteTracker::Combine(Effect::Draw, Effect::Unknown) == Effect::Unknown);
    tracker.Entry(Effect::Gpu, 5);
    tracker.DrawEpoch(3);
    tracker.Entry(Effect::Draw, 8);
    tracker.DrawEpoch(3);
    tracker.Entry(Effect::Draw, 10);
    CHECK(tracker.ReadTarget(0x1000, 0x1004) == 10);
    CHECK(tracker.WriterReadTarget(0x1000, 0x1004) == 5);
    CHECK(tracker.LastGpu() == 10);
    CHECK(tracker.DrawsValid(5, 3));
    CHECK_FALSE(tracker.DrawsValid(5, 4));
    // A Draw entry sent without an epoch never validates.
    tracker.Entry(Effect::Draw, 12);
    CHECK_FALSE(tracker.DrawsValid(10, 3));
    tracker.Retire(12);
    CHECK(tracker.DrawsValid(12, 4));
    // A noted write still holds the reads of its range back.
    tracker.Write(0x1000, 0x1100, 14);
    CHECK(tracker.WriterReadTarget(0x1000, 0x1004) == 14);
    CHECK(tracker.AnyPendingWrite(12, 14, [](u64 begin, u64) { return begin == 0x1000; }));
    CHECK_FALSE(tracker.AnyPendingWrite(14, 20, [](u64, u64) { return true; }));
    tracker.Entry(Effect::Gpu, 16);
    CHECK(tracker.WriterReadTarget(0x8000, 0x8004) == 16);
}

TEST_CASE("Split write tracker collapses too many writes into an unknown one",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    u64 position = 0;
    for (size_t i = 0; i <= SplitWriteTracker::MaxWrites; ++i) {
        tracker.Write(i * 0x2000, i * 0x2000 + 0x1000, ++position);
    }
    CHECK(tracker.LastUnknown() == position);
    CHECK(tracker.ReadTarget(0x100000000, 0x100001000) == position);
}

TEST_CASE("Split DMA read waits for a first CPU-side copy after the last sync",
          "[video_core][split_writes][split_correctness]") {
    SplitWriteTracker tracker;
    constexpr u64 copy_position = 20;
    constexpr u64 last_sync = 10;
    tracker.Entry(Effect::None, last_sync);
    tracker.Entry(Effect::Gpu, copy_position);
    u32 guest_word = 0;
    const u32 copied_word = 0x12345678;
    const bool page_gpu_written = false;
    const u64 target = tracker.ReadTarget(0x1000, 0x1004);
    if (target >= copy_position) {
        guest_word = copied_word;
    }
    REQUIRE_FALSE(page_gpu_written);
    CHECK(target >= copy_position);
    CHECK(guest_word == copied_word);
}

TEST_CASE("Split DMA page checks cannot exclude a queued first GPU writer",
          "[video_core][split_writes][split_correctness]") {
    SplitWriteTracker tracker;
    constexpr u64 dispatch_position = 30;
    tracker.Entry(Effect::Gpu, dispatch_position);
    bool page_gpu_written = false;
    const bool marked_before_wait = page_gpu_written;
    const u64 target = tracker.ReadTarget(0x8000, 0x8004);
    if (target >= dispatch_position) {
        page_gpu_written = true;
    }
    REQUIRE_FALSE(marked_before_wait);
    CHECK(page_gpu_written);
}

TEST_CASE("Split write tracker matches a model of every queued entry",
          "[video_core][split_writes]") {
    std::mt19937_64 rng{0x5eed};
    for (int run = 0; run < 200; ++run) {
        SplitWriteTracker tracker;
        Model model;
        u64 position = 0;
        u64 consumed = 0;
        for (int step = 0; step < 400; ++step) {
            const u64 roll = rng() % 100;
            if (roll < 55) {
                // An entry: what it does, maybe with a write to a few pages.
                position += 1 + rng() % 8;
                const u64 kind = rng() % 10;
                const Effect effect = kind < 6 ? Effect::None
                                      : kind < 9 ? Effect::Gpu
                                                 : Effect::Unknown;
                tracker.Entry(effect, position);
                u64 begin = 0;
                u64 end = 0;
                if (rng() % 2 == 0) {
                    begin = (rng() % 64) * 0x1000;
                    end = begin + (1 + rng() % 3) * 0x1000;
                    tracker.Write(begin, end, position);
                }
                model.entries.push_back({position, effect, begin, end});
            } else if (roll < 65) {
                tracker.Entry(Effect::None, ++position);
                model.entries.push_back({position, Effect::None, 0, 0});
            } else if (roll < 80) {
                // The back moves on.
                consumed = (std::min)(position, consumed + rng() % 32);
                tracker.Retire(consumed);
            } else {
                const u64 begin = (rng() % 72) * 0x1000 + rng() % 0x1000;
                const u64 end = begin + 4 + rng() % 0x2000;
                const u64 expected = model.Target(begin, end, consumed);
                const u64 target = tracker.ReadTarget(begin, end);
                // Never earlier than needed (the read would miss a write)...
                REQUIRE((target <= consumed ? 0 : target) >= expected);
                // ...never past the last entry, and no wait once the back ran everything.
                REQUIRE(target <= position);
                if (consumed == position) {
                    REQUIRE(target <= consumed);
                }
            }
        }
    }
}
