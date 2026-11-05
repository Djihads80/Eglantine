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

    // The last position the back must pass: unknown writes, GPU work at or before the last
    // synchronization, and writes to [begin, end); entries it consumed don't count.
    u64 Target(u64 begin, u64 end, u64 last_sync, u64 consumed) const {
        u64 target = 0;
        for (const Entry& entry : entries) {
            if (entry.position <= consumed) {
                continue;
            }
            const bool needed = entry.effect == Effect::Unknown ||
                                (entry.effect == Effect::Gpu && entry.position <= last_sync) ||
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

TEST_CASE("Split write tracker waits for GPU work only up to the last sync",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    tracker.Entry(Effect::Gpu, 5);
    tracker.Entry(Effect::None, 6);
    tracker.Entry(Effect::Gpu, 9);
    // No sync since the work: nothing to wait for.
    CHECK(tracker.ReadTarget(0x1000, 0x1004, 0) == 0);
    // A sync after the first draw only.
    CHECK(tracker.ReadTarget(0x1000, 0x1004, 6) == 6);
    // A sync after both.
    CHECK(tracker.ReadTarget(0x1000, 0x1004, 12) == 9);
    tracker.Entry(Effect::Unknown, 14);
    CHECK(tracker.ReadTarget(0x1000, 0x1004, 0) == 14);
}

TEST_CASE("Split write tracker collapses too many writes into an unknown one",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker;
    u64 position = 0;
    for (size_t i = 0; i <= SplitWriteTracker::MaxWrites; ++i) {
        tracker.Write(i * 0x2000, i * 0x2000 + 0x1000, ++position);
    }
    CHECK(tracker.LastUnknown() == position);
    CHECK(tracker.ReadTarget(0x100000000, 0x100001000, 0) == position);
}

TEST_CASE("Split write tracker matches a model of every queued entry",
          "[video_core][split_writes]") {
    std::mt19937_64 rng{0x5eed};
    for (int run = 0; run < 200; ++run) {
        SplitWriteTracker tracker;
        Model model;
        u64 position = 0;
        u64 consumed = 0;
        u64 last_sync = 0;
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
                last_sync = position;
            } else if (roll < 80) {
                // The back moves on.
                consumed = (std::min)(position, consumed + rng() % 32);
                tracker.Retire(consumed);
            } else {
                const u64 begin = (rng() % 72) * 0x1000 + rng() % 0x1000;
                const u64 end = begin + 4 + rng() % 0x2000;
                const u64 expected = model.Target(begin, end, last_sync, consumed);
                const u64 target = tracker.ReadTarget(begin, end, last_sync);
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
