// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "core/device_memory.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2_perf.h"

namespace {

using Runs = std::vector<std::pair<VAddr, size_t>>;

constexpr u64 PAGE = Core::DEVICE_PAGESIZE;

class RecordingTracker {
public:
    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
        batches.push_back({Runs{{addr, size}}, delta});
    }

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        batches.push_back({Runs(ranges.begin(), ranges.end()), delta});
    }

    std::vector<std::pair<Runs, s32>> batches;
};

using Manager = VideoCommon::WordManager<RecordingTracker, 16, 4ULL << 20>;

// WordManager::ApplyCollectedRanges' merge before Nx2Perf.
Runs OldWordCoalesce(Runs ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Runs coalesced;
    VAddr cur_addr = ranges[0].first;
    size_t cur_size = ranges[0].second;
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (cur_addr + cur_size == ranges[i].first) {
            cur_size += ranges[i].second;
        } else {
            coalesced.emplace_back(cur_addr, cur_size);
            cur_addr = ranges[i].first;
            cur_size = ranges[i].second;
        }
    }
    coalesced.emplace_back(cur_addr, cur_size);
    return coalesced;
}

// DeviceMemoryManager::UpdatePagesCachedBatch's sort and merge before Nx2Perf.
Runs OldBatchCoalesce(const Runs& ranges) {
    Runs tmp(ranges.begin(), ranges.end());
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Runs coalesced;
    DAddr cur_addr = tmp[0].first;
    size_t cur_size = tmp[0].second;
    for (size_t i = 1; i < tmp.size(); ++i) {
        if (cur_addr + cur_size >= tmp[i].first) {
            const DAddr end = std::max(cur_addr + cur_size, tmp[i].first + tmp[i].second);
            cur_size = end - cur_addr;
        } else {
            coalesced.emplace_back(cur_addr, cur_size);
            cur_addr = tmp[i].first;
            cur_size = tmp[i].second;
        }
    }
    coalesced.emplace_back(cur_addr, cur_size);
    return coalesced;
}

bool Separated(const Runs& runs) {
    return std::ranges::adjacent_find(runs, [](const auto& a, const auto& b) {
               return a.first > b.first || a.first + a.second >= b.first;
           }) == runs.end();
}

// What CollectChangedRanges gathers over a few words: page runs of random masks, in word order.
Runs RandomCollected(std::mt19937_64& rng, VAddr base) {
    Manager::CollectedRanges ranges;
    const size_t words = 1 + rng() % 6;
    for (size_t word = 0; word < words; ++word) {
        u64 mask = rng();
        switch (rng() % 4) {
        case 0:
            mask = ~u64{0};
            break;
        case 1:
            mask &= rng();
            break;
        case 2:
            mask = u64{1} << (rng() % 64);
            break;
        default:
            break;
        }
        const VAddr addr = base + word * VideoCommon::BYTES_PER_WORD;
        size_t offset = 0;
        while (mask != 0) {
            const size_t empty = std::countr_zero(mask);
            offset += empty;
            mask >>= empty;
            const size_t ones = std::countr_one(mask);
            ranges.emplace_back(addr + offset * PAGE, ones * PAGE);
            mask = ones < 64 ? mask >> ones : 0;
            offset += ones;
        }
    }
    std::shuffle(ranges.begin(), ranges.end(), rng);
    return Runs(ranges.begin(), ranges.end());
}

} // Anonymous namespace

TEST_CASE("Nx2Perf texture-writes: switches can be disabled independently", "[video_core]") {
    using namespace VideoCommon::Nx2Perf;
    const char* master = std::getenv("EDEN_NX2_PERF_OPT");
    const char* disabled = std::getenv("EDEN_NX2_PERF_OFF");
    const bool enabled = master == nullptr || master[0] != '0';
    const std::string list = "," + std::string{disabled ? disabled : ""} + ",";
    REQUIRE(On(Opt::WriteIntersections) ==
            (enabled && list.find(",write_intersections,") == std::string::npos));
    REQUIRE(On(Opt::SortedPageBatches) ==
            (enabled && list.find(",sorted_page_batches,") == std::string::npos));
}

TEST_CASE("Nx2Perf texture-writes: in-place tracker merge matches the old copy", "[video_core]") {
    std::mt19937_64 rng{0x0400486024DEC000ULL};
    for (int step = 0; step < 100000; ++step) {
        const Runs raw = RandomCollected(rng, (rng() % 64) * VideoCommon::BYTES_PER_WORD);
        if (raw.empty()) {
            continue;
        }
        const Runs old = OldWordCoalesce(raw);
        Manager::CollectedRanges ranges(raw.begin(), raw.end());
        std::sort(ranges.begin(), ranges.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        Manager::CoalesceInPlace(ranges);
        REQUIRE(Runs(ranges.begin(), ranges.end()) == old);
        RecordingTracker recorder;
        Manager manager{0, recorder};
        Manager::CollectedRanges collected(raw.begin(), raw.end());
        const s32 delta = step % 2 == 0 ? 1 : -1;
        manager.ApplyCollectedRanges(collected, delta);
        REQUIRE(collected.empty());
        REQUIRE(recorder.batches.size() == 1);
        REQUIRE(recorder.batches.front().first == old);
        REQUIRE(recorder.batches.front().second == delta);
        // The device manager takes these as they are: its old sort and merge leave them alone.
        REQUIRE(Separated(old));
        REQUIRE(OldBatchCoalesce(old) == old);
    }
}

TEST_CASE("Nx2Perf texture-writes: batch fast path only takes runs its merge would keep", "[video_core]") {
    std::mt19937_64 rng{0x5EEDULL};
    for (int step = 0; step < 100000; ++step) {
        Runs runs;
        const size_t count = 1 + rng() % 8;
        for (size_t i = 0; i < count; ++i) {
            runs.emplace_back((rng() % 64) * PAGE, (rng() % 4) * PAGE);
        }
        if (rng() % 2 == 0) {
            std::sort(runs.begin(), runs.end());
        }
        if (Separated(runs)) {
            REQUIRE(OldBatchCoalesce(runs) == runs);
        }
    }
}

TEST_CASE("Nx2Perf texture-writes: tracker batches reach the device manager separated", "[video_core]") {
    std::mt19937_64 rng{0xE2CULL};
    RecordingTracker recorder;
    VideoCommon::MemoryTrackerBase<RecordingTracker> tracker{recorder};
    constexpr VAddr base = 16ULL << 22;
    constexpr u64 span = 12ULL << 20;
    for (int step = 0; step < 50000; ++step) {
        const VAddr addr = base + (rng() % (span / 64)) * 64;
        const u64 size = 1 + rng() % (rng() % 2 == 0 ? 8192 : (3ULL << 20));
        switch (rng() % 5) {
        case 0:
        case 1:
            tracker.MarkRegionAsCpuModified(addr, size);
            break;
        case 2:
            tracker.ForEachUploadRange(addr, size, [](u64, u64) {});
            break;
        case 3:
            tracker.CachedCpuWrite(addr, size);
            break;
        default:
            tracker.FlushCachedWrites();
            break;
        }
    }
    REQUIRE(!recorder.batches.empty());
    for (const auto& [runs, delta] : recorder.batches) {
        REQUIRE(!runs.empty());
        REQUIRE(Separated(runs));
        REQUIRE(OldBatchCoalesce(runs) == runs);
    }
}

TEST_CASE("Nx2Perf texture-writes: page lookup answers both GPU-written checks", "[video_core]") {
    std::mt19937_64 rng{0x0400D4201C6EA000ULL};
    Common::RangeSet<DAddr> gpu_modified;
    constexpr DAddr base = 1ULL << 32;
    for (int step = 0; step < 200000; ++step) {
        const DAddr addr = base + rng() % (4ULL << 20);
        const u64 size = rng() % 4 == 0 ? 0 : 1 + rng() % (rng() % 2 == 0 ? 256 : 65536);
        switch (rng() % 4) {
        case 0:
            gpu_modified.Add(addr, size);
            continue;
        case 1:
            gpu_modified.Subtract(addr, size);
            continue;
        default:
            break;
        }
        const DAddr page_begin = Common::AlignDown(addr, PAGE);
        const DAddr page_end = Common::AlignUp(addr + size, PAGE);
        if (!gpu_modified.Intersects(page_begin, page_end - page_begin)) {
            REQUIRE_FALSE(gpu_modified.Intersects(addr, size));
        }
    }
}

TEST_CASE("Nx2Perf texture-writes: device manager batch accepts tracker runs", "[video_core]") {
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager manager(device_memory);
    const std::vector<std::pair<DAddr, size_t>> separated{{0, PAGE}, {PAGE * 3, PAGE * 2}};
    manager.UpdatePagesCachedBatch(separated, 1);
    manager.UpdatePagesCachedBatch(separated, -1);
    const std::vector<std::pair<DAddr, size_t>> touching{{PAGE, PAGE}, {0, PAGE}};
    manager.UpdatePagesCachedBatch(touching, 1);
    manager.UpdatePagesCachedBatch(touching, -1);
    SUCCEED();
}
