// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.inc"
#include "video_core/buffer_cache/cpu_write_batch.h"
#include "video_core/buffer_cache/proven_compute_writes.h"
#include "video_core/native_proven_writes.h"

namespace {
namespace PCW = VideoCommon::ProvenComputeWrites;
namespace SWB = Shader::StorageWriteBounds;
constexpr DAddr Base = 16 * 1024 * 1024;
constexpr GPUVAddr Gpu = 32 * 1024 * 1024;
using Range = std::pair<DAddr, u32>;

SWB::DescriptorProof Proof() {
    SWB::DescriptorProof proof;
    proof.cbuf_index = 0;
    proof.cbuf_offset = 0x150;
    proof.binding_alignment = 16;
    proof.complete = true;
    proof.nodes = {{SWB::Op::Constant, 16}, {SWB::Op::Constant, 128}};
    proof.stores = {{0, 8, 8}, {1, 8, 8}};
    return proof;
}
constexpr SWB::Dispatch Dispatch{{1, 1, 1}, {64, 1, 1}, true};
constexpr PCW::Binding Binding{Base, Gpu, 4096, true, PCW::Coverage::FullMapping};

struct Fixture {
    PCW::Plan plan;
    SWB::DescriptorProof proof = Proof();
    std::array<PCW::Binding, 2> bindings{Binding, {}};
    std::vector<Range> marked;
    Fixture() { plan.Prepare(0, Binding, &proof, Dispatch, 16); }
    PCW::Result Apply(bool other_images = false) {
        return plan.Apply(0, bindings, other_images,
            [&](DAddr address, u32 bytes) { marked.emplace_back(address, bytes); });
    }
};
} // namespace

TEST_CASE("Proven compute writes apply only the proven union through the real policy",
          "[video_core][proven_writes]") {
    Fixture fixture;
    const auto result = fixture.Apply();
    CHECK(result.reason == PCW::Reason::Proven);
    CHECK(fixture.marked == std::vector<Range>{{Base + 16, 8}, {Base + 128, 8}});
    CHECK(result.original_bytes == 4096);
    CHECK(result.marked_bytes == 16);
    CHECK(result.ranges == 2);
    CHECK(fixture.bindings[0].device_addr == Base);
    CHECK(fixture.bindings[0].gpu_addr == Gpu);
    CHECK(fixture.bindings[0].size == 4096); // The helper never modifies the full binding.
}

TEST_CASE("Writable physical aliases fall back even with disjoint GPU virtual addresses",
          "[video_core][proven_writes]") {
    Fixture fixture;
    fixture.bindings[1] = {Base + 2048, Gpu + 8 * 1024 * 1024, 4096, true, PCW::Coverage::FullMapping};
    const auto result = fixture.Apply();
    CHECK(result.reason == PCW::Reason::WritableAlias);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("Read-only aliases and adjacent writable bindings do not invent extra writes",
          "[video_core][proven_writes]") {
    Fixture fixture;
    SECTION("Read-only alias") {
        fixture.bindings[1] = {Base, Gpu + 8 * 1024 * 1024, 4096, false, PCW::Coverage::None};
    }
    SECTION("Adjacent writable binding") {
        fixture.bindings[1] = {Base + 4096, Gpu + 4096, 4096, true, PCW::Coverage::FullMapping};
    }
    CHECK(fixture.Apply().reason == PCW::Reason::Proven);
    CHECK(fixture.marked.size() == 2);
}

TEST_CASE("Indirect, shape, alignment and incomplete proof retain the full binding",
          "[video_core][proven_writes]") {
    Fixture fixture;
    auto dispatch = Dispatch;
    PCW::Reason expected = PCW::Reason::MissingProof;
    u32 alignment = 16;
    SECTION("Indirect or mismatched pipeline local dimensions") {
        dispatch.direct = false;
        expected = PCW::Reason::IndirectOrShapeMismatch;
    }
    SECTION("Shader has an unknown additional writer") { fixture.proof.complete = false; }
    SECTION("Pipeline and current binding alignment differ") {
        alignment = 256;
        expected = PCW::Reason::AlignmentMismatch;
    }
    SECTION("One store exceeds the binding; never clip it") {
        fixture.proof.nodes[1].a = 4096;
        expected = PCW::Reason::UnprovedBounds;
    }
    fixture.plan.Prepare(0, Binding, &fixture.proof, dispatch, alignment);
    CHECK(fixture.Apply().reason == expected);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("A current mapping change, sparse binding or another image writer fails closed",
          "[video_core][proven_writes]") {
    Fixture fixture;
    PCW::Reason expected = PCW::Reason::BindingChanged;
    bool images = false;
    SECTION("Physical remap") { fixture.bindings[0].device_addr += 8192; }
    SECTION("GPU remap") { fixture.bindings[0].gpu_addr += 8192; }
    SECTION("Binding extent changed") { fixture.bindings[0].size = 8192; }
    SECTION("Current range is sparse or gathered") {
        fixture.bindings[0].coverage = PCW::Coverage::None;
        expected = PCW::Reason::NonContiguous;
    }
    SECTION("Another writable range is sparse and could alias through another segment") {
        fixture.bindings[1] = {Base + 8192, Gpu + 8192, 4096, true, PCW::Coverage::None};
        expected = PCW::Reason::NonContiguous;
    }
    SECTION("Other image writes") { images = true; expected = PCW::Reason::OtherImageWrites; }
    CHECK(fixture.Apply(images).reason == expected);
    CHECK(fixture.marked == std::vector<Range>{{fixture.bindings[0].device_addr,
                                               fixture.bindings[0].size}});
}

TEST_CASE("A new dispatch cannot inherit an old compute proof", "[video_core][proven_writes]") {
    Fixture fixture;
    fixture.plan.Reset();
    CHECK_FALSE(fixture.plan.HasCandidate());
    CHECK(fixture.Apply().reason == PCW::Reason::MissingProof);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
    fixture.marked.clear();
    fixture.plan.Prepare(0, Binding, &fixture.proof, Dispatch, 16);
    fixture.plan.Prepare(0, Binding, nullptr, Dispatch, 16);
    CHECK_FALSE(fixture.plan.HasCandidate());
    CHECK(fixture.Apply().reason == PCW::Reason::MissingProof);
}

TEST_CASE("Zero direct work is distinct from unknown writes", "[video_core][proven_writes]") {
    Fixture fixture;
    auto empty_dispatch = Dispatch;
    empty_dispatch.groups[0] = 0;
    fixture.plan.Prepare(0, Binding, &fixture.proof, empty_dispatch, 16);
    const auto result = fixture.Apply();
    CHECK(result.reason == PCW::Reason::Proven);
    CHECK(result.marked_bytes == 0);
    CHECK(fixture.marked.empty());
}

TEST_CASE("Bounded physical ranges reject overflow", "[video_core][proven_writes]") {
    CHECK_FALSE(PCW::Valid({(u64{1} << 36) - 2, Gpu, 8, true, PCW::Coverage::FullMapping}));
    CHECK_FALSE(PCW::Valid({Base, std::numeric_limits<u64>::max() - 2, 8, true, PCW::Coverage::FullMapping}));
    CHECK_FALSE(PCW::Valid({0, Gpu, 4096, true, PCW::Coverage::FullMapping}));
    CHECK_FALSE(PCW::Valid({Base, Gpu, 0, true, PCW::Coverage::FullMapping}));
}

TEST_CASE("Proven writes retain unrelated dirty bytes and pending snapshots across CPU writes",
          "[video_core][readback][proven_writes]") {
    Fixture fixture;
    Common::RangeSet<DAddr> gpu_written;
    Common::RangeSet<DAddr> pending;
    gpu_written.Add(Base + 2000, 16);
    pending.Add(Base + 16, 8); // Invalidated by the new GPU write.
    pending.Add(Base + 1000, 16); // Unrelated older snapshot must survive.
    const auto result = fixture.plan.Apply(0, fixture.bindings, false,
        [&](DAddr address, u32 bytes) {
            // Real MarkWrittenBuffer's dirty/pending operations, fed ONLY through
            // the same production planning seam. No replacement download algorithm.
            pending.Subtract(address, bytes);
            gpu_written.Add(address, bytes);
        });
    REQUIRE(result.reason == PCW::Reason::Proven);
    CHECK_FALSE(pending.Intersects(Base + 16, 8));
    CHECK(pending.Intersects(Base + 1000, 16));
    CHECK(gpu_written.Intersects(Base + 2000, 16));
    CHECK_FALSE(gpu_written.Intersects(Base + 3000, 16));
    const std::array<VideoCommon::CpuWriteRange, 1> cpu_writes{{{Base + 20, 4}}};
    VideoCommon::WritePreserveDownloadHints hints;
    const auto pages = VideoCommon::PrepareCpuWriteBatch(cpu_writes, gpu_written, hints,
        [&](DAddr address, u64 bytes) { pending.Subtract(address, bytes); });
    CHECK(pages.Intersects(Base, 4096));
    CHECK(gpu_written.Intersects(Base + 16, 4));
    CHECK_FALSE(gpu_written.Intersects(Base + 20, 4));
    CHECK(gpu_written.Intersects(Base + 128, 8));
    CHECK(gpu_written.Intersects(Base + 2000, 16));
    CHECK(pending.Intersects(Base + 1000, 16));
}

TEST_CASE("Activation is strict opt-in and counters report actual marked bytes",
          "[video_core][proven_writes]") {
    CHECK(VideoCommon::NativeProvenWrites::GateEnabled("1"));
    for (const auto value : {"", "0", "true", "01", "1 "})
        CHECK_FALSE(VideoCommon::NativeProvenWrites::GateEnabled(value));
    PCW::Counters counters;
    Fixture fixture;
    counters.Add(fixture.Apply());
    fixture.plan.Reset();
    counters.Add(fixture.Apply());
    CHECK(counters.calls == 2);
    CHECK(counters.proven == 1);
    CHECK(counters.original_bytes == 8192);
    CHECK(counters.marked_bytes == 4096 + 16);
    CHECK(counters.ranges == 3);
}

namespace {
struct MappingProbe {
    struct Call {
        GPUVAddr gpu;
        DAddr expected;
        u32 size;
        auto operator<=>(const Call&) const = default;
    };
    std::vector<Call> calls;
    std::optional<GPUVAddr> broken_byte;
    bool reject_translation{};
    bool operator()(GPUVAddr gpu, DAddr expected, u32 size) {
        calls.push_back({gpu, expected, size});
        if (reject_translation) return false;
        return !broken_byte || *broken_byte < gpu || *broken_byte - gpu >= size;
    }
};
}

TEST_CASE("Range-map gate is strict and depends on the parent proven-writes gate",
          "[video_core][proven_writes][proven_range_map]") {
    CHECK(VideoCommon::NativeProvenWrites::RangeMapGateEnabled(true, "1"));
    CHECK_FALSE(VideoCommon::NativeProvenWrites::RangeMapGateEnabled(false, "1"));
    for (const auto value : {"", "0", "true", "01", "1 "})
        CHECK_FALSE(VideoCommon::NativeProvenWrites::RangeMapGateEnabled(true, value));
}

TEST_CASE("Range mapping checks the current complete write union, not gaps or old mapping results",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    MappingProbe mapping;
    SECTION("Unmapped base before the first store") { mapping.broken_byte = Gpu; }
    SECTION("Remapped gap between stores") { mapping.broken_byte = Gpu + 64; }
    SECTION("Unmapped tail after all stores") { mapping.broken_byte = Gpu + 2048; }
    CHECK(fixture.plan.ValidateMapping(0, Binding, true, false, mapping) == PCW::Coverage::None);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu, Base, 4096}});
    mapping.calls.clear();
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    CHECK(fixture.bindings[0].coverage == PCW::Coverage::ProvenWriteRanges);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu + 16, Base + 16, 8},
                                                         {Gpu + 128, Base + 128, 8}});
    CHECK(fixture.Apply().reason == PCW::Reason::Proven);
    CHECK(fixture.marked == std::vector<Range>{{Base + 16, 8}, {Base + 128, 8}});
    // The same proof is checked again against the NOW changed writer mapping.
    mapping.calls.clear();
    mapping.broken_byte = Gpu + 131;
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    fixture.marked.clear();
    CHECK(fixture.bindings[0].coverage == PCW::Coverage::None);
    CHECK(mapping.calls.size() == 2);
    CHECK(fixture.Apply().reason == PCW::Reason::NonContiguous);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("Range mapping validates rounded envelopes including unknown alignment bias",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    fixture.proof.nodes = {{SWB::Op::Constant, 31}, {SWB::Op::AlignmentBias, 256},
                           {SWB::Op::AddU32, 0, 1}};
    fixture.proof.stores = {{2, 4, 4}};
    fixture.plan.Prepare(0, Binding, &fixture.proof, Dispatch, 16);
    MappingProbe mapping;
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu + 28, Base + 28, 262}});
    CHECK(fixture.Apply().reason == PCW::Reason::Proven);
    CHECK(fixture.marked == std::vector<Range>{{Base + 28, 262}});
    // Outside the literal offset but inside its required alignment envelope.
    mapping.broken_byte = Gpu + 28;
    CHECK(fixture.plan.ValidateMapping(0, Binding, true, true, mapping) == PCW::Coverage::None);
}

TEST_CASE("Range mapping rejects a different physical translation even when the bytes are linear",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    MappingProbe mapping;
    mapping.reject_translation = true;
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu + 16, Base + 16, 8}});
    CHECK(fixture.Apply().reason == PCW::Reason::NonContiguous);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("Unknown writable peers still require full mapping and invalidate a narrow writer",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    fixture.bindings[1] = {Base + 8192, Gpu + 8192, 4096, true, PCW::Coverage::None};
    MappingProbe mapping;
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    mapping.calls.clear();
    mapping.broken_byte = Gpu + 8192 + 2048;
    fixture.bindings[1].coverage = fixture.plan.ValidateMapping(1, fixture.bindings[1], true,
                                                              true, mapping);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu + 8192, Base + 8192, 4096}});
    CHECK(fixture.bindings[1].coverage == PCW::Coverage::None);
    CHECK(fixture.Apply().reason == PCW::Reason::NonContiguous);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
    // Merely labelling an unknown peer partially covered cannot bypass the guard.
    fixture.bindings[1].coverage = PCW::Coverage::ProvenWriteRanges;
    CHECK(fixture.Apply().reason == PCW::Reason::NonContiguous);
}

TEST_CASE("Partially verified bindings retain full-span conservative physical alias rejection",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    fixture.bindings[1] = {Base + 2048, Gpu + 8192, 4096, true, PCW::Coverage::None};
    fixture.plan.Prepare(1, fixture.bindings[1], &fixture.proof, Dispatch, 16);
    MappingProbe mapping;
    for (std::size_t index = 0; index < fixture.bindings.size(); ++index) {
        fixture.bindings[index].coverage = fixture.plan.ValidateMapping(
            index, fixture.bindings[index], true, true, mapping);
        REQUIRE(fixture.bindings[index].coverage == PCW::Coverage::ProvenWriteRanges);
    }
    // Even disjoint store envelopes do not relax the original full-span alias policy.
    CHECK(fixture.Apply().reason == PCW::Reason::WritableAlias);
    CHECK(fixture.marked == std::vector<Range>{{Base, 4096}});
    fixture.bindings[1].device_addr = Base + 8192;
    fixture.plan.Prepare(1, fixture.bindings[1], &fixture.proof, Dispatch, 16);
    fixture.bindings[1].coverage = fixture.plan.ValidateMapping(1, fixture.bindings[1], true,
                                                              true, mapping);
    CHECK(fixture.Apply().reason == PCW::Reason::Proven);
}

TEST_CASE("Range mapping requires current host eligibility and matching dispatch identity",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    MappingProbe mapping;
    SECTION("Null sparse gathered or out-of-host-buffer binding") {
        CHECK(fixture.plan.ValidateMapping(0, Binding, false, true, mapping) == PCW::Coverage::None);
        CHECK(mapping.calls.empty());
    }
    SECTION("Invalid device range") {
        auto changed = Binding;
        changed.device_addr = (u64{1} << 36) - 1;
        CHECK(fixture.plan.ValidateMapping(0, changed, true, true, mapping) == PCW::Coverage::None);
        CHECK(mapping.calls.empty());
    }
    SECTION("Identity changed since proof preparation") {
        fixture.bindings[0].device_addr += 8192;
        fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, fixture.bindings[0], true,
                                                                  true, mapping);
        CHECK(fixture.bindings[0].coverage == PCW::Coverage::FullMapping);
        CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu, Base + 8192, 4096}});
        CHECK(fixture.Apply().reason == PCW::Reason::BindingChanged);
    }
    SECTION("Indirect shape retains full validation") {
        auto indirect = Dispatch;
        indirect.direct = false;
        fixture.plan.Prepare(0, Binding, &fixture.proof, indirect, 16);
        CHECK(fixture.plan.ValidateMapping(0, Binding, true, true, mapping) == PCW::Coverage::FullMapping);
        CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu, Base, 4096}});
    }
}

TEST_CASE("Only a proved zero direct dispatch can validate an empty union without mapping reads",
          "[video_core][proven_writes][proven_range_map]") {
    Fixture fixture;
    auto empty = Dispatch;
    empty.groups[0] = 0;
    fixture.plan.Prepare(0, Binding, &fixture.proof, empty, 16);
    MappingProbe mapping;
    mapping.reject_translation = true;
    fixture.bindings[0].coverage = fixture.plan.ValidateMapping(0, Binding, true, true, mapping);
    CHECK(fixture.bindings[0].coverage == PCW::Coverage::ProvenWriteRanges);
    CHECK(mapping.calls.empty());
    CHECK(fixture.Apply().reason == PCW::Reason::Proven);
    CHECK(fixture.marked.empty());
    fixture.plan.Reset();
    CHECK(fixture.plan.ValidateMapping(0, Binding, true, true, mapping) == PCW::Coverage::None);
    CHECK(mapping.calls == std::vector<MappingProbe::Call>{{Gpu, Base, 4096}});
    CHECK(fixture.Apply().reason == PCW::Reason::MissingProof);
}

#include "video_core/buffer_cache/proven_compute_sync.h"
namespace {
namespace PCS = VideoCommon::ProvenComputeSync;
struct SyncFixture {
    PCS::Plan plan;
    SWB::DescriptorAccessProof access{Proof()};
    std::array<PCS::Binding, 2> bindings{{{Base, Gpu, 4096, true}, {}}};
    std::vector<Range> synchronized;
    SyncFixture() { plan.Prepare(0, bindings[0], access, Dispatch, 16); }
    void Ready() {
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            if (!bindings[index].enabled) continue;
            bindings[index].coverage = plan.Preflight(index, bindings[index], true,
                [](GPUVAddr, DAddr, u32) { return true; },
                [](DAddr, u64) { return true; });
        }
    }
    PCS::Outcome Apply(std::size_t index = 0, bool images = false) {
        return plan.Apply(index, bindings, images,
            [&](DAddr address, u32 size) { synchronized.emplace_back(address, size); });
    }
};
}

TEST_CASE("Joint sync preflight validates complete envelopes and host page expansion before use",
          "[video_core][proven_sync]") {
    SyncFixture fixture;
    std::vector<Range> mapped;
    std::vector<std::pair<DAddr, u64>> host_checks;
    fixture.bindings[0].coverage = fixture.plan.Preflight(0, fixture.bindings[0], true,
        [&](GPUVAddr gpu, DAddr expected, u32 size) {
            CHECK(gpu - Gpu == expected - Base);
            mapped.emplace_back(expected, size);
            return true;
        },
        [&](DAddr address, u64 size) { host_checks.emplace_back(address, size); return true; });
    CHECK(fixture.synchronized.empty());
    CHECK(mapped == std::vector<Range>{{Base + 16, 8}, {Base + 128, 8}});
    CHECK(host_checks == std::vector<std::pair<DAddr, u64>>{{Base, 4096}, {Base, 4096}});
    CHECK(fixture.Apply() == PCS::Outcome::Narrow);
    CHECK(fixture.synchronized == mapped);
}

TEST_CASE("Joint sync preflight failure never partially consumes uploads",
          "[video_core][proven_sync]") {
    SyncFixture fixture;
    bool eligible = true;
    bool host_fit = true;
    bool mapping_ok = true;
    SECTION("Sparse gathered null or out-of-host-range") { eligible = false; }
    SECTION("Page expansion does not fit even if shader bytes fit") { host_fit = false; }
    SECTION("Second envelope is remapped") { mapping_ok = false; }
    fixture.bindings[0].coverage = fixture.plan.Preflight(0, fixture.bindings[0], eligible,
        [&](GPUVAddr, DAddr address, u32) { return address == Base + 16 || mapping_ok; },
        [&](DAddr, u64) { return host_fit; });
    CHECK(fixture.synchronized.empty());
    CHECK(fixture.Apply() == PCS::Outcome::Full);
    CHECK(fixture.synchronized == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("Joint sync includes unknown read-only peers in full mapping and alias decisions",
          "[video_core][proven_sync]") {
    SyncFixture fixture;
    fixture.bindings[1] = {Base + 8192, Gpu + 8192, 4096, true};
    fixture.Ready();
    REQUIRE(fixture.bindings[1].coverage == PCS::Coverage::FullMapping);
    SECTION("Unknown input stays full while independent proved output narrows") {
        CHECK(fixture.Apply() == PCS::Outcome::Narrow);
        fixture.synchronized.clear();
        CHECK(fixture.Apply(1) == PCS::Outcome::Full);
        CHECK(fixture.synchronized == std::vector<Range>{{Base + 8192, 4096}});
    }
    SECTION("Unknown peer not mapped") {
        fixture.bindings[1].coverage = PCS::Coverage::None;
        CHECK(fixture.Apply() == PCS::Outcome::Full);
    }
    SECTION("Forged partial coverage on unknown input") {
        fixture.bindings[1].coverage = PCS::Coverage::ReadWriteRanges;
        CHECK(fixture.Apply() == PCS::Outcome::Full);
    }
    SECTION("Read-only input aliases the full physical output span") {
        fixture.bindings[1].device_addr = Base + 2048;
        CHECK(fixture.Apply() == PCS::Outcome::Full);
    }
}

TEST_CASE("Joint sync never inherits incomplete indirect mismatched or reset proofs",
          "[video_core][proven_sync]") {
    SyncFixture fixture;
    SECTION("Incomplete access proof") {
        fixture.access.bounds.complete = false;
        fixture.plan.Prepare(0, fixture.bindings[0], fixture.access, Dispatch, 16);
    }
    SECTION("Indirect dispatch") {
        auto indirect = Dispatch;
        indirect.direct = false;
        fixture.plan.Prepare(0, fixture.bindings[0], fixture.access, indirect, 16);
    }
    SECTION("Alignment mismatch") { fixture.plan.Prepare(0, fixture.bindings[0], fixture.access, Dispatch, 256); }
    SECTION("Current binding identity differs") { fixture.bindings[0].gpu_addr += 4096; }
    SECTION("New dispatch reset") { fixture.plan.Reset(); }
    fixture.Ready();
    CHECK(fixture.Apply() == PCS::Outcome::Full);
    CHECK(fixture.synchronized == std::vector<Range>{{Base, 4096}});
}

TEST_CASE("Joint sync gate and image fallback remain conservative", "[video_core][proven_sync]") {
    CHECK(VideoCommon::NativeProvenWrites::SyncGateEnabled(true, "1"));
    CHECK_FALSE(VideoCommon::NativeProvenWrites::SyncGateEnabled(false, "1"));
    for (const auto value : {"", "0", "true", "01", "1 "})
        CHECK_FALSE(VideoCommon::NativeProvenWrites::SyncGateEnabled(true, value));
    SyncFixture fixture;
    fixture.Ready();
    CHECK(fixture.Apply(0, true) == PCS::Outcome::Full);
    auto empty = Dispatch;
    empty.groups[0] = 0;
    fixture.plan.Prepare(0, fixture.bindings[0], fixture.access, empty, 16);
    fixture.Ready();
    fixture.synchronized.clear();
    CHECK(fixture.Apply() == PCS::Outcome::Narrow);
    CHECK(fixture.synchronized.empty());
}

TEST_CASE("Global-read write proofs require their own strict opt-in gate",
          "[proven_writes][global_read_write_bounds]") {
    CHECK(VideoCommon::NativeProvenWrites::GlobalReadsGateEnabled(true, "1"));
    CHECK_FALSE(VideoCommon::NativeProvenWrites::GlobalReadsGateEnabled(false, "1"));
    for (const auto value : {"", "0", "true", "01", "1 "}) {
        CHECK_FALSE(VideoCommon::NativeProvenWrites::GlobalReadsGateEnabled(true, value));
    }
}
