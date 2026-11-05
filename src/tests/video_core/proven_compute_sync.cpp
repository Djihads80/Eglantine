// SPDX-License-Identifier: GPL-3.0-or-later
#include <map>
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include "video_core/buffer_cache/memory_tracker_base.h"
namespace {
constexpr u64 PAGE = 4096;
constexpr DAddr c = 16 * (1ULL << 22);
// Only the page-reference bookkeeping dependency is faked. MemoryTracker and
// the upload/GPU-write subtraction below are the actual production helpers.
struct RasterizerInterface {
    std::map<u64, int> references;
    void UpdatePagesCachedCount(DAddr address, size_t size, s32 delta) {
        for (u64 page = address / PAGE; page < (address + size + PAGE - 1) / PAGE; ++page) {
            int& count = references[page];
            count += delta;
            if (count < 0) throw std::logic_error("negative cached page count");
            if (!count) references.erase(page);
        }
    }
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        for (const auto& [address, size] : ranges) UpdatePagesCachedCount(address, size, delta);
    }
};
using MemoryTracker = VideoCommon::MemoryTrackerBase<RasterizerInterface>;
}

#include "video_core/native_proven_sync_audit.h"

TEST_CASE("Sync audit counts request spans without calling them transferred bytes",
          "[video_core][proven_sync_audit]") {
    namespace Audit = VideoCommon::NativeProvenSyncAudit;
    using Outcome = VideoCommon::ProvenComputeSync::Outcome;
    CHECK(Audit::GateEnabled(true, "1"));
    CHECK_FALSE(Audit::GateEnabled(false, "1"));
    for (const auto value : {"", "0", "true", "01", "1 "})
        CHECK_FALSE(Audit::GateEnabled(true, value));
    Audit::Counters counts;
    CHECK_FALSE(counts.Observe(Outcome::Full, 8 << 20, 8 << 20));
    CHECK_FALSE(counts.Observe(Outcome::Narrow, 8 << 20, 16384));
    CHECK_FALSE(counts.Observe(Outcome::Narrow, 8 << 20, 0));
    CHECK(counts.calls == 3);
    CHECK(counts.full == 1);
    CHECK(counts.narrow == 2);
    CHECK(counts.reference_full_span_bytes == 3 * (8 << 20));
    CHECK(counts.requested_span_bytes == (8 << 20) + 16384);
    CHECK(counts.narrow_requested_span_bytes == 16384);
    // Exercise the real cadence, without clocks, logging or a game process.
    unsigned reports{};
    for (unsigned i = 3; i < 131072; ++i)
        if (counts.Observe(Outcome::Full, 4096, 4096)) ++reports;
    CHECK(reports == 3); // 1024,65536,131072; no report for every binding.
}
#include "common/range_sets.inc"
#include "video_core/buffer_cache/proven_compute_sync.h"
#include "video_core/buffer_cache/cpu_upload_ranges.h"

TEST_CASE("Proven sync uses real page tracking and preserves GPU bytes plus future dirty pages",
          "[video_core][proven_sync]") {
    namespace PCS = VideoCommon::ProvenComputeSync;
    namespace SWB = Shader::StorageWriteBounds;
    RasterizerInterface rasterizer;
    MemoryTracker tracker(rasterizer);
    Common::RangeSet<DAddr> gpu_written;
    Common::RangeSet<DAddr> uploaded;
    tracker.UnmarkRegionAsCpuModified(c, 4 * PAGE);
    tracker.MarkRegionAsCpuModified(c + 20, 1);
    tracker.MarkRegionAsCpuModified(c + 2000, 1); // Same dirty page, outside every shader access.
    tracker.MarkRegionAsCpuModified(c + 3 * PAGE + 50, 1); // Must remain pending for a later user.
    gpu_written.Add(c + 128, 8); // A current read must see newer host-GPU bytes, not CPU memory.
    gpu_written.Add(c + 3000, 20); // Another GPU write outside the access envelope on the same page.
    SWB::DescriptorAccessProof proof;
    proof.bounds.complete = true;
    proof.bounds.binding_alignment = 16;
    proof.bounds.nodes = {{SWB::Op::Constant, 16}, {SWB::Op::Constant, 128}};
    proof.bounds.stores = {{0, 8, 8}, {1, 8, 8}};
    std::array<PCS::Binding, 1> bindings{{{c, c * 2, 4 * PAGE, true}}};
    PCS::Plan plan;
    plan.Prepare(0, bindings[0], proof, {{1, 1, 1}, {64, 1, 1}, true}, 16);
    bindings[0].coverage = plan.Preflight(0, bindings[0], true,
        [](GPUVAddr, DAddr, u32) { return true; },
        [](DAddr address, u64 size) { return address >= c && size <= c + 4 * PAGE - address; });
    auto synchronize = [&](DAddr address, u32 size) {
        VideoCommon::ForEachCpuUploadExcludingGpu(tracker, gpu_written, address, size,
            [&](DAddr first, DAddr end) { uploaded.Add(first, end - first); });
    };
    REQUIRE(plan.Apply(0, bindings, false, synchronize) == PCS::Outcome::Narrow);
    CHECK(uploaded.Intersects(c, 128));
    CHECK(uploaded.Intersects(c + 2000, 1));
    CHECK_FALSE(uploaded.Intersects(c + 128, 8));
    CHECK_FALSE(uploaded.Intersects(c + 3000, 20));
    CHECK_FALSE(uploaded.Intersects(c + 3 * PAGE, PAGE));
    CHECK_FALSE(tracker.IsRegionCpuModified(c, PAGE));
    CHECK(tracker.IsRegionCpuModified(c + 3 * PAGE, PAGE));
    CHECK(gpu_written.Intersects(c + 128, 8));
    CHECK(gpu_written.Intersects(c + 3000, 20));
    // An unrelated later pipeline's full sync must still upload the untouched dirty page.
    plan.Reset();
    REQUIRE(plan.Apply(0, bindings, false, synchronize) == PCS::Outcome::Full);
    CHECK(uploaded.Intersects(c + 3 * PAGE, PAGE));
    CHECK_FALSE(tracker.IsRegionCpuModified(c + 3 * PAGE, PAGE));
    CHECK_FALSE(uploaded.Intersects(c + 128, 8));
    CHECK_FALSE(uploaded.Intersects(c + 3000, 20));
    CHECK(gpu_written.Intersects(c + 128, 8));
    CHECK(gpu_written.Intersects(c + 3000, 20));
}
