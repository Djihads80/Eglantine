// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "video_core/nx2_perf.h"
#include "video_core/split_write_tracker.h"

namespace {

using VideoCommon::SplitConstBufferWords;
using VideoCommon::SplitRing;
using VideoCommon::SplitRingMode;
using VideoCommon::SplitWriteTracker;
using Effect = SplitWriteTracker::Effect;

// Nx2Perf SplitRings off, on, and both containers compared (EDEN_NX2_PERF_VERIFY).
#define RING_MODE GENERATE(SplitRingMode::Off, SplitRingMode::On, SplitRingMode::Verify)

SplitWriteTracker MakeTracker(SplitRingMode mode) {
    SplitWriteTracker tracker;
    tracker.SetRings(mode);
    return tracker;
}

u64 RingMismatches() {
    return VideoCommon::g_split_ring_mismatches.load();
}

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

TEST_CASE("Split overlap stays disabled until asynchronous write ordering is proven",
          "[video_core][split_writes][split_correctness]") {
    CHECK_FALSE(VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::SplitOverlap));
}

TEST_CASE("Split write tracker adds, overlaps and retires writes", "[video_core][split_writes]") {
    const u64 mismatches = RingMismatches();
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    CHECK(RingMismatches() == mismatches);
}

TEST_CASE("Split write tracker waits for every unlocalized writer",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    const u64 mismatches = RingMismatches();
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    CHECK(RingMismatches() == mismatches);
}

TEST_CASE("Split write tracker keeps write kinds apart (MacroReadOverlayWriters)",
          "[video_core][split_writes]") {
    using Kind = SplitWriteTracker::WriteKind;
    const SplitRingMode mode = RING_MODE;
    const u64 mismatches = RingMismatches();
    SplitWriteTracker tracker = MakeTracker(mode);
    tracker.SetSeparateKinds(true);
    tracker.Write(0x1000, 0x2000, 3, Kind::ConstantBuffer);
    tracker.Write(0x2000, 0x3000, 5);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000, Kind::Other) == 0);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000, Kind::ConstantBuffer) == 3);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000) == 3);
    CHECK(tracker.LastWriteTo(0x2000, 0x3000, Kind::Other) == 5);
    CHECK(tracker.LastWriteTo(0x1000, 0x3000) == 5);
    // Same kinds still join: an Other write over the constant buffer page joins the Other
    // record, and the constant buffer record keeps its kind.
    tracker.Write(0x3000, 0x4000, 6);
    CHECK(tracker.LastWriteTo(0x2000, 0x3000, Kind::Other) == 6);
    tracker.Write(0x1000, 0x2000, 7);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000, Kind::Other) == 7);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000, Kind::ConstantBuffer) == 3);
    tracker.Retire(3);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000, Kind::ConstantBuffer) == 0);
    CHECK(tracker.LastWriteTo(0x1000, 0x2000) == 7);
    tracker.Retire(7);
    CHECK(tracker.LastWriteTo(0x1000, 0x4000) == 0);

    // Off: the old rule, one merged record of kind Other.
    SplitWriteTracker merged = MakeTracker(mode);
    merged.Write(0x1000, 0x2000, 3, Kind::ConstantBuffer);
    merged.Write(0x2000, 0x3000, 5);
    CHECK(merged.LastWriteTo(0x1000, 0x2000, Kind::Other) == 5);
    CHECK(merged.LastWriteTo(0x1000, 0x2000, Kind::ConstantBuffer) == 0);
    CHECK(RingMismatches() == mismatches);
}

TEST_CASE("Split write tracker skips only proven draws for writer reads",
          "[video_core][split_writes]") {
    const u64 mismatches = RingMismatches();
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    CHECK(RingMismatches() == mismatches);
}

TEST_CASE("Split write tracker collapses too many writes into an unknown one",
          "[video_core][split_writes]") {
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
    u64 position = 0;
    for (size_t i = 0; i <= SplitWriteTracker::MaxWrites; ++i) {
        tracker.Write(i * 0x2000, i * 0x2000 + 0x1000, ++position);
    }
    CHECK(tracker.LastUnknown() == position);
    CHECK(tracker.ReadTarget(0x100000000, 0x100001000) == position);
    CHECK(tracker.PendingWrites() == 0);
}

TEST_CASE("Split DMA read waits for a first CPU-side copy after the last sync",
          "[video_core][split_writes][split_correctness]") {
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    SplitWriteTracker tracker = MakeTracker(RING_MODE);
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
    const SplitRingMode mode = RING_MODE;
    const u64 mismatches = RingMismatches();
    std::mt19937_64 rng{0x5eed};
    for (int run = 0; run < 200; ++run) {
        SplitWriteTracker tracker = MakeTracker(mode);
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
    CHECK(RingMismatches() == mismatches);
}

TEST_CASE("Split ring keeps std::deque order across wraps and growth",
          "[video_core][split_writes]") {
    std::mt19937_64 rng{0x41ee};
    SplitRing<u64, 4096> fixed;
    SplitRing<u64, 8, true> growing;
    std::deque<u64> fixed_ref;
    std::deque<u64> growing_ref;
    const auto compare = [](const auto& ring, const std::deque<u64>& ref, bool whole) {
        REQUIRE(ring.size() == ref.size());
        REQUIRE(ring.empty() == ref.empty());
        if (ref.empty()) {
            return;
        }
        REQUIRE(ring.front() == ref.front());
        REQUIRE(ring.back() == ref.back());
        if (!whole) {
            return;
        }
        size_t index = 0;
        for (const u64 value : ring) {
            REQUIRE(value == ref[index]);
            REQUIRE(ring[index] == ref[index]);
            ++index;
        }
        REQUIRE(index == ref.size());
        auto it = ref.rbegin();
        for (auto ring_it = ring.rbegin(); ring_it != ring.rend(); ++ring_it, ++it) {
            REQUIRE(*ring_it == *it);
        }
        REQUIRE(it == ref.rend());
    };
    u64 value = 0;
    for (int step = 0; step < 40000; ++step) {
        // Phases of filling and draining, so both rings wrap at every fill level.
        const bool filling = (step / 3000) % 2 == 0;
        if (rng() % 100 < (filling ? 80u : 25u)) {
            ++value;
            if (fixed.size() < 4096) {
                fixed.push_back(value);
                fixed_ref.push_back(value);
            }
            growing.push_back(value);
            growing_ref.push_back(value);
        } else {
            if (!fixed_ref.empty()) {
                fixed.pop_front();
                fixed_ref.pop_front();
            }
            if (!growing_ref.empty()) {
                growing.pop_front();
                growing_ref.pop_front();
            }
        }
        if (step % 1000 == 0 && rng() % 4 == 0) {
            fixed.back() = ++value;
            fixed_ref.back() = value;
        }
        const bool whole = step % 97 == 0 || fixed.size() == 4096;
        compare(fixed, fixed_ref, whole);
        compare(growing, growing_ref, whole);
    }
    REQUIRE(fixed_ref.size() <= 4096);
    fixed.clear();
    REQUIRE(fixed.empty());
}

TEST_CASE("Split rings answer as the std::deque past their capacities",
          "[video_core][split_writes]") {
    using Kind = SplitWriteTracker::WriteKind;
    const u64 mismatches = RingMismatches();
    std::mt19937_64 rng{0x7a1e};
    std::vector<SplitWriteTracker> trackers(3);
    trackers[1].SetRings(SplitRingMode::On);
    trackers[2].SetRings(SplitRingMode::Verify);
    const auto all = [&trackers](auto&& apply) {
        for (SplitWriteTracker& tracker : trackers) {
            apply(tracker);
        }
    };
    u64 position = 0;
    u64 consumed = 0;
    u64 epoch = 1;
    size_t most_writes = 0;
    bool overflowed = false;
    for (int step = 0; step < 60000; ++step) {
        // Fill without retiring (past MaxWrites and the epoch ring's first capacity), then drain
        // part of it, so the rings wrap and grow with entries pending.
        const int phase = (step / 5000) % 4;
        const u64 roll = rng() % 100;
        if (roll < 45) {
            position += 1 + rng() % 4;
            // Mostly apart, sometimes next to the last one (joins it).
            const u64 begin = (rng() % 4096) * 0x1000 + (rng() % 2) * 0x800;
            const u64 end = begin + 0x10 + rng() % 0x1000;
            const Kind kind = rng() % 3 == 0 ? Kind::ConstantBuffer : Kind::Other;
            const bool separate = rng() % 2 == 0;
            all([&](SplitWriteTracker& tracker) {
                tracker.SetSeparateKinds(separate);
                tracker.Write(begin, end, position, kind);
                tracker.Entry(Effect::None, position);
            });
        } else if (roll < 75) {
            ++position;
            if (phase == 2 || rng() % 8 == 0) {
                ++epoch;
            }
            const bool with_epoch = rng() % 16 != 0;
            const Effect effect = rng() % 12 == 0 ? Effect::Gpu
                                  : rng() % 40 == 0 ? Effect::Unknown
                                                    : Effect::Draw;
            all([&](SplitWriteTracker& tracker) {
                if (with_epoch) {
                    tracker.DrawEpoch(epoch);
                }
                tracker.Entry(effect, position);
            });
        } else if (roll < 85) {
            if (phase == 1 || phase == 3) {
                consumed = (std::min)(position, consumed + rng() % 96);
                all([&](SplitWriteTracker& tracker) { tracker.Retire(consumed); });
            }
        } else {
            const u64 begin = (rng() % 4100) * 0x1000 + rng() % 0x1000;
            const u64 end = begin + 4 + rng() % 0x3000;
            const Kind kind = rng() % 2 == 0 ? Kind::ConstantBuffer : Kind::Other;
            const SplitWriteTracker& reference = trackers[0];
            const auto touches = [begin, end](u64 write_begin, u64 write_end) {
                return write_begin < end && begin < write_end;
            };
            for (size_t i = 1; i < trackers.size(); ++i) {
                const SplitWriteTracker& tracker = trackers[i];
                INFO("step " << step << " tracker " << i);
                REQUIRE(tracker.PendingWrites() == reference.PendingWrites());
                REQUIRE(tracker.LastUnknown() == reference.LastUnknown());
                REQUIRE(tracker.LastWriteTo(begin, end) == reference.LastWriteTo(begin, end));
                REQUIRE(tracker.LastWriteTo(begin, end, kind) ==
                        reference.LastWriteTo(begin, end, kind));
                REQUIRE(tracker.ReadTarget(begin, end) == reference.ReadTarget(begin, end));
                REQUIRE(tracker.WriterReadTarget(begin, end) ==
                        reference.WriterReadTarget(begin, end));
                REQUIRE(tracker.DrawsValid(consumed, epoch) ==
                        reference.DrawsValid(consumed, epoch));
                REQUIRE(tracker.DrawsValid(consumed, epoch - 1) ==
                        reference.DrawsValid(consumed, epoch - 1));
                REQUIRE(tracker.AnyPendingWrite(consumed, tracker.LastGpu(), touches) ==
                        reference.AnyPendingWrite(consumed, reference.LastGpu(), touches));
            }
        }
        most_writes = (std::max)(most_writes, trackers[0].PendingWrites());
        overflowed = overflowed || trackers[0].LastUnknown() != 0;
    }
    CHECK(most_writes >= SplitWriteTracker::MaxWrites - 1);
    CHECK(overflowed);
    CHECK(RingMismatches() == mismatches);

    // Switching takes the pending writes and draw epochs over.
    SplitWriteTracker& tracker = trackers[0];
    const u64 last = tracker.LastWriteTo(0, ~u64{0});
    const bool valid = tracker.DrawsValid(consumed, epoch);
    tracker.SetRings(SplitRingMode::On);
    CHECK(tracker.LastWriteTo(0, ~u64{0}) == last);
    CHECK(tracker.DrawsValid(consumed, epoch) == valid);
    tracker.SetRings(SplitRingMode::Off);
    CHECK(tracker.LastWriteTo(0, ~u64{0}) == last);
    CHECK(tracker.DrawsValid(consumed, epoch) == valid);
}

TEST_CASE("Split constant buffer word rings overlay as the std::deque",
          "[video_core][split_writes]") {
    const u64 mismatches = RingMismatches();
    std::mt19937_64 rng{0xcb0d};
    std::vector<SplitConstBufferWords> queues(3);
    queues[1].SetRings(SplitRingMode::On);
    queues[2].SetRings(SplitRingMode::Verify);
    u64 position = 0;
    u64 consumed = 0;
    size_t most_words = 0;
    for (int step = 0; step < 30000; ++step) {
        const int phase = (step / 1500) % 2;
        const u64 roll = rng() % 100;
        if (roll < 50) {
            // A run of words, kept only while the queue has room (GpuSplit::NoteConstBufferWrite).
            ++position;
            const u32 count = 1 + static_cast<u32>(rng() % 64);
            const u64 device = (rng() % 0x4000) * 4;
            if (queues[0].Size() + count <= SplitConstBufferWords::MaxWords) {
                for (u32 i = 0; i < count; ++i) {
                    const u32 value = static_cast<u32>(rng());
                    for (SplitConstBufferWords& queue : queues) {
                        queue.Push({device + u64{i} * 4, position, value});
                    }
                }
            }
        } else if (roll < 70) {
            if (phase == 1) {
                consumed = (std::min)(position, consumed + rng() % 16);
                for (SplitConstBufferWords& queue : queues) {
                    queue.Retire(consumed);
                }
            }
        } else {
            const u64 begin = rng() % 0x10010;
            const u64 size = 1 + rng() % 0x100;
            const u64 at = rng() % (position + 1);
            std::vector<u8> reference(size);
            for (u8& byte : reference) {
                byte = static_cast<u8>(rng());
            }
            const std::vector<u8> base = reference;
            queues[0].Overlay(at, begin, size, reference.data());
            for (size_t i = 1; i < queues.size(); ++i) {
                INFO("step " << step << " queue " << i);
                std::vector<u8> out = base;
                queues[i].Overlay(at, begin, size, out.data());
                REQUIRE(out == reference);
                REQUIRE(queues[i].Size() == queues[0].Size());
            }
        }
        most_words = (std::max)(most_words, queues[0].Size());
    }
    CHECK(most_words > SplitConstBufferWords::MaxWords - 64);
    CHECK(RingMismatches() == mismatches);
}
