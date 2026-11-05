// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "video_core/nx2_command_writer_trace.h"
namespace {
namespace W = VideoCommon::CommandWriterTrace;
}
TEST_CASE("Command writer ranges clip exact bytes without overflow", "[command_writer_trace]") {
    auto hit = W::Intersect({0x1003, 8}, {0x1000, 4096});
    REQUIRE(hit);
    CHECK(hit->address == 0x1003);
    CHECK(hit->size == 8);
    hit = W::Intersect({0x1ffc, 8}, {0x2000, 4096});
    REQUIRE(hit);
    CHECK(hit->address == 0x2000);
    CHECK(hit->size == 4);
    CHECK_FALSE(W::Intersect({0x1000, 4096}, {0x2000, 4096}));
    CHECK_FALSE(W::Intersect({0x1000, 0}, {0x1000, 4096}));
    CHECK_FALSE(W::Intersect({~u64{0} - 3, 8}, {0, 4096}));
}
TEST_CASE("Command writer registry distinguishes GPU ASIDs and physical device aliases",
          "[command_writer_trace]") {
    W::Registry registry;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55));
    REQUIRE(registry.Add(2, 0x1000, 0xa000, 55));
    REQUIRE(registry.Add(1, 0x2000, 0x9000, 55));
    REQUIRE(registry.Add(1, 0x1000, 0xb000, 55)); // Retain both old and new backing.
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55)); // Dedupe same mapping.
    CHECK(registry.Watches().size() == 4);
    REQUIRE(registry.Add(3, 0x3000, 0, 55));
    size_t count{};
    registry.Gpu(1, {0x1004, 4}, [&](const auto &, auto overlap) {
        CHECK(overlap.address == 0x1004);
        CHECK(overlap.size == 4);
        ++count;
    });
    CHECK(count == 2);
    count = 0;
    registry.Device(55, {0x9008, 4}, [&](const auto &, auto) { ++count; });
    CHECK(count == 2); // Same device bytes back two GPU VAs.
    count = 0;
    registry.Device(99, {0x9008, 4}, [&](const auto &, auto) { ++count; });
    CHECK(count == 0);
    registry.Device(0, {0x9008, 4}, [&](const auto &, auto) { ++count; });
    CHECK(count == 2); // Global dirty bitmap declaration has no manager cookie.
}
TEST_CASE("Command writer watches remain bounded and reject malformed page aliases",
          "[command_writer_trace]") {
    W::Registry registry;
    CHECK_FALSE(registry.Add(1, 1, 0x1000, 1));
    CHECK_FALSE(registry.Add(1, ~u64{0} & ~u64{4095}, 0x1000, 1));
    CHECK_FALSE(registry.Add(1, 0x1000, ~u64{0} & ~u64{4095}, 1));
    for (size_t i = 0; i < W::MaxOrdinaryWatches; ++i)
        REQUIRE(registry.Add(1, (i + 1) * 4096, (i + 5) * 4096, 1));
    CHECK_FALSE(registry.Add(1, 0x100000, 0x200000, 1));
    CHECK(registry.Watches().size() == W::MaxOrdinaryWatches);
    CHECK(registry.Dropped() == 1);
    CHECK(registry.DroppedOrdinary() == 1);
    CHECK(registry.DroppedTargeted() == 0);
    CHECK(registry.Add(1, 4096, 5 * 4096, 1));
}
TEST_CASE("Command writer history owns metadata and keeps the latest bounded events",
          "[command_writer_trace]") {
    W::Ring<2> ring;
    W::Event event{.ns = 1,
                   .watch_id = 7,
                   .kind = W::Kind::Snapshot,
                   .submission = 18,
                   .entry_index = 90,
                   .descriptor = 0x1234,
                   .suspension_epoch = 6,
                   .words = {0x2004c004, 0x21, 0x1000}};
    ring.Append(event);
    event.words[0] = 0;
    CHECK(ring.Snapshot()[0].words[0] == 0x2004c004);
    event.ns = 2;
    ring.Append(event);
    event.ns = 3;
    ring.Append(event);
    const auto events = ring.Snapshot();
    REQUIRE(events.size() == 2);
    CHECK(events[0].ns == 2);
    CHECK(events[1].ns == 3);
    CHECK(events[1].submission == 18);
    CHECK(events[1].entry_index == 90);
    CHECK(ring.Total() == 3);
}


namespace {
struct WriterContextRestore {
    W::CpuContext context = W::cpu_context;
    u64 present = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    ~WriterContextRestore() {
        W::cpu_context = context;
        Core::g_nx2_present_count.store(present, std::memory_order_relaxed);
    }
};
void CheckContext(u64 pid, u64 core, u64 pc) {
    CHECK(W::cpu_context.pid == pid);
    CHECK(W::cpu_context.core == core);
    CHECK(W::cpu_context.block_pc == pc);
}
}

TEST_CASE("CPU writer trace does not collect context outside its active window",
          "[command_writer_trace][writer_scope]") {
    WriterContextRestore restore;
    W::cpu_context = {11, 22, 33};
    unsigned reads = 0;
    for (const auto present : {W::FirstPresent - 1, W::LastPresent + 1}) {
        Core::g_nx2_present_count.store(present, std::memory_order_relaxed);
        {
            W::CpuScope scope{[&] {
                ++reads;
                return W::CpuContext{44, 55, 66};
            }};
            CheckContext(11, 22, 33);
        }
        CheckContext(11, 22, 33);
    }
    CHECK(reads == 0);
}

TEST_CASE("CPU writer trace context collection is lazy and nested scopes restore exactly",
          "[command_writer_trace][writer_scope]") {
    WriterContextRestore restore;
    W::cpu_context = {11, 22, 33};
    Core::g_nx2_present_count.store(W::FirstPresent, std::memory_order_relaxed);
    unsigned reads = 0;
    const bool enabled = W::Enabled();
    {
        W::CpuScope outer{[&] {
            ++reads;
            return W::CpuContext{44, 55, 66};
        }};
        CHECK(reads == (enabled ? 1U : 0U));
        CheckContext(enabled ? 44 : 11, enabled ? 55 : 22, enabled ? 66 : 33);
        {
            W::CpuScope inner{[&] {
                ++reads;
                return W::CpuContext{77, 88, 99};
            }};
            CheckContext(enabled ? 77 : 11, enabled ? 88 : 22, enabled ? 99 : 33);
        }
        CheckContext(enabled ? 44 : 11, enabled ? 55 : 22, enabled ? 66 : 33);
    }
    CheckContext(11, 22, 33);
    CHECK(reads == (enabled ? 2U : 0U));
}

TEST_CASE("CPU writer context restores on unwind and a failed collector cannot install context",
          "[command_writer_trace][writer_scope]") {
    WriterContextRestore restore;
    W::cpu_context = {11, 22, 33};
    Core::g_nx2_present_count.store(W::FirstPresent, std::memory_order_relaxed);
    CHECK_THROWS_AS(([&] {
        W::CpuScope scope{[] { return W::CpuContext{44, 55, 66}; }};
        throw 1;
    }()), int);
    CheckContext(11, 22, 33);
    unsigned reads = 0;
    bool threw = false;
    try {
        W::CpuScope scope{[&]() -> W::CpuContext {
            ++reads;
            throw 2;
        }};
    } catch (int) {
        threw = true;
    }
    CHECK(threw == W::Enabled());
    CHECK(reads == (W::Enabled() ? 1U : 0U));
    CheckContext(11, 22, 33);
}

TEST_CASE("Command writer descriptor selection is narrow across both graphics channels",
          "[command_writer_trace]") {
    for (const s32 channel : {1, 2}) {
        CHECK(W::WatchesDescriptor(channel, 1));
        CHECK(W::WatchesDescriptor(channel, 3));
        CHECK(W::WatchesDescriptor(channel, 5));
        CHECK(W::WatchesDescriptor(channel, 8));
        CHECK_FALSE(W::WatchesDescriptor(channel, 0));
        CHECK_FALSE(W::WatchesDescriptor(channel, 9));
        CHECK_FALSE(W::WatchesDescriptor(channel, 16));
    }
    for (const s32 channel : {-1, 0, 3, 4, 5, 6}) {
        CHECK_FALSE(W::WatchesDescriptor(channel, 3));
    }
    W::Event event{.kind = W::Kind::Snapshot, .entry_words = 8, .snapshot_channel = 1,
                   .words = {1, 2, 3}};
    W::Ring<1> history;
    history.Append(event);
    event.words.fill(0);
    const auto retained = history.Snapshot();
    REQUIRE(retained.size() == 1);
    CHECK(retained[0].entry_words == 8);
    CHECK(retained[0].snapshot_channel == 1);
    CHECK(retained[0].words == std::array<u32, 3>{1, 2, 3});
}

TEST_CASE("Targeted command descriptors reserve capacity without widening small-entry selection",
          "[command_writer_trace]") {
    CHECK(W::IsTargetedDescriptor(2, 13));
    CHECK(W::WatchesDescriptor(2, 13));
    for (const s32 channel : {-1, 0, 1, 3, 4, 5, 6}) {
        CHECK_FALSE(W::IsTargetedDescriptor(channel, 13));
        CHECK_FALSE(W::WatchesDescriptor(channel, 13));
    }
    for (const u64 words : {0, 9, 10, 11, 12, 14, 15, 16, 17}) {
        CHECK_FALSE(W::WatchesDescriptor(2, words));
    }
    W::Registry registry;
    for (size_t i = 0; i < W::MaxOrdinaryWatches; ++i) {
        REQUIRE(registry.Add(1, (i + 1) * W::PageSize, (i + 1) * W::PageSize, 5));
    }
    CHECK_FALSE(registry.Add(1, 0x100000, 0x200000, 5));
    for (size_t i = 0; i < W::ReservedTargetedWatches; ++i) {
        REQUIRE(registry.Add(1, 0x100000 + i * W::PageSize, 0x200000 + i * W::PageSize, 5,
                             true));
    }
    CHECK(registry.Watches().size() == W::MaxWatches);
    CHECK(registry.OrdinaryCount() == W::MaxOrdinaryWatches);
    CHECK(registry.TargetedCount() == W::ReservedTargetedWatches);
    CHECK_FALSE(registry.Add(1, 0x300000, 0x400000, 5, true));
    CHECK(registry.DroppedOrdinary() == 1);
    CHECK(registry.DroppedTargeted() == 1);
    CHECK(registry.Dropped() == 2);
    // The watched52-byte descriptor straddles pages; both physical aliases are already retained.
    size_t covered{};
    registry.Gpu(1, {0x100ffc, 13 * sizeof(u32)}, [&](const W::Watch& w, W::Range overlap) {
        CHECK(w.targeted);
        covered += overlap.size;
    });
    CHECK(covered == 13 * sizeof(u32));
}

TEST_CASE("Targeted watch promotion retains IDs and alias ownership within the global bound",
          "[command_writer_trace]") {
    W::Registry registry;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55));
    const auto id = registry.Watches()[0].id;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55, true));
    CHECK(registry.Watches().size() == 1);
    CHECK(registry.Watches()[0].id == id);
    CHECK(registry.OrdinaryCount() == 0);
    CHECK(registry.TargetedCount() == 1);
    // A new mapping inherits its class; the old physical alias remains observable.
    const auto old = registry.Watches()[0];
    REQUIRE(registry.Add(old.gpu_asid, old.gpu_page, 0xa000, old.owner, old.targeted));
    CHECK(registry.TargetedCount() == 2);
    size_t hits{};
    registry.Gpu(1, {0x1004, 4}, [&](const W::Watch& w, W::Range) {
        CHECK(w.targeted);
        ++hits;
    });
    CHECK(hits == 2);
    for (size_t i = registry.Watches().size(); i < W::MaxWatches; ++i) {
        REQUIRE(registry.Add(2, (i + 1) * W::PageSize, (i + 1) * W::PageSize, 55, true));
    }
    CHECK_FALSE(registry.Add(2, 0x200000, 0x300000, 55, true));
    CHECK(registry.Watches().size() == W::MaxWatches);
    CHECK(registry.DroppedTargeted() == 1);
    CHECK(registry.DroppedOrdinary() == 0);
}
