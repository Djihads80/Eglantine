// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/atomic_ops.h"
#include "video_core/nx2_cpu_write_trace.h"

namespace W = VideoCommon::CommandWriterTrace;

TEST_CASE("CPU completed trace observes real stores only after their operation",
          "[command_writer_trace][cpu_write_trace]") {
    W::Registry registry;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55));
    u64 destination = 7;
    const u64 source = 37;
    std::vector<W::Event> events;
    unsigned prepares{}, stores{}, publications{};
    const auto prepare = [&] {
        ++prepares;
        CHECK(destination == 7);
        CHECK(events.empty());
        W::CpuWriteObservation observation;
        observation.Capture(registry, 55, {0x9008, sizeof(destination)}, 0x5008,
                            reinterpret_cast<u64>(&destination), {81, 2, 0x1234});
        return observation;
    };
    CHECK(W::ObserveCpuWrite(true, prepare, [&] {
        ++stores;
        CHECK(events.empty());
        std::memcpy(&destination, &source, sizeof(destination));
        return true;
    }, [&](auto& observation, bool committed) {
        ++publications;
        CHECK(destination == 37);
        observation.Complete(committed, 100, 3301, [&](auto event) { events.push_back(event); });
    }));
    CHECK(prepares == 1);
    CHECK(stores == 1);
    CHECK(publications == 1);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == W::Kind::CpuCompletedWrite);
    CHECK(events[0].phase == W::Phase::After);
    CHECK(events[0].range.address == 0x9008);
    CHECK(events[0].range.size == sizeof(destination));
    CHECK(events[0].cpu.block_pc == 0x1234);
    CHECK(events[0].words == std::array<u32, 3>{}); // No memory values are exported.
}

TEST_CASE("CPU completed trace rejects failed actual CAS and preserves the result",
          "[command_writer_trace][cpu_write_trace]") {
    W::Registry registry;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55));
    u64 destination = 7;
    std::vector<W::Event> events;
    const auto execute = [&](u64 expected) {
        return W::ObserveCpuWrite(true, [&] {
            W::CpuWriteObservation observation;
            observation.Capture(registry, 55, {0x9000, 8}, 0x5000,
                                reinterpret_cast<u64>(&destination), {});
            return observation;
        }, [&] {
            return Common::AtomicCompareAndSwap(&destination, u64{37}, expected);
        }, [&](auto& observation, bool committed) {
            observation.Complete(committed, 100, 3301, [&](auto event) { events.push_back(event); });
        });
    };
    CHECK_FALSE(execute(6));
    CHECK(destination == 7);
    CHECK(events.empty());
    CHECK(execute(7));
    CHECK(destination == 37);
    REQUIRE(events.size() == 1);
}

TEST_CASE("CPU completed observation owns aliases and context and publishes at most once",
          "[command_writer_trace][cpu_write_trace]") {
    W::Registry registry;
    REQUIRE(registry.Add(1, 0x1000, 0x9000, 55));
    REQUIRE(registry.Add(1, 0x2000, 0xa000, 55));
    W::CpuWriteObservation observation;
    observation.Capture(registry, 55, {0x9ffc, 4}, 0x5ffc, 0x10005ffc, {81, 3, 0x1234});
    observation.Capture(registry, 55, {0xa000, 4}, 0x6000, 0x10006000, {81, 3, 0x1234});
    observation.Capture(registry, 55, {0x9ffc, 4}, 0x5ffc, 0x10005ffc, {}); // Duplicate.
    REQUIRE(registry.Add(1, 0x1000, 0xb000, 55)); // New mapping is not retroactively the writer.
    std::vector<W::Event> events;
    observation.Complete(true, 101, 3302, [&](auto event) { events.push_back(event); });
    observation.Complete(true, 102, 3303, [&](auto event) { events.push_back(event); });
    REQUIRE(events.size() == 2);
    CHECK(events[0].range.address == 0x9ffc);
    CHECK(events[1].range.address == 0xa000);
    CHECK(events[0].cpu.core == 3);
    CHECK(events[0].cpu.block_pc == 0x1234);
    CHECK(events[0].ns == 101);
    CHECK(events[0].present == 3302);
    W::CpuWriteObservation missing;
    missing.Capture(registry, 99, {0x9000, 8}, 0, 0, {});
    CHECK(missing.Empty());
    missing.Capture(registry, 55, {0xc000, 8}, 0, 0, {});
    CHECK(missing.Empty());
}

TEST_CASE("Disabled CPU completed trace performs only the original store",
          "[command_writer_trace][cpu_write_trace]") {
    for (const bool result : {false, true}) {
        unsigned prepared{}, stores{}, published{};
        CHECK(W::ObserveCpuWrite(false, [&] {
            ++prepared;
            return W::CpuWriteObservation{};
        }, [&] {
            ++stores;
            return result;
        }, [&](auto&, bool) { ++published; }) == result);
        CHECK(prepared == 0);
        CHECK(stores == 1);
        CHECK(published == 0);
    }
}
