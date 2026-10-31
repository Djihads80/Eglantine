// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

using Service::Nvidia::Devices::Nx2GpuMemoryRegistry;

namespace {

struct Event {
    char kind;
    u64 gpu;
    u64 cpu;
    u64 size;

    bool operator==(const Event&) const = default;
};

class Recorder {
public:
    explicit Recorder(std::vector<Event>& events_) : events{events_} {
        Nx2GpuMemoryRegistry::Get().AddListener(
            this, [this](u64 gpu, u64 cpu, u64 size) { events.push_back({'m', gpu, cpu, size}); },
            [this](u64 gpu, u64 size) { events.push_back({'u', gpu, 0, size}); },
            [this](u64 cpu, u64 size) { events.push_back({'r', 0, cpu, size}); },
            [this](u64 gpu, u64 size, bool create) {
                events.push_back({create ? 's' : 'd', gpu, 0, size});
            },
            [this](u64 gpu, u64 cpu, u64 size) { events.push_back({'b', gpu, cpu, size}); },
            [this](u64 gpu, u64 size) { events.push_back({'v', gpu, 0, size}); });
    }

    ~Recorder() {
        Nx2GpuMemoryRegistry::Get().RemoveListener(this);
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

private:
    std::vector<Event>& events;
};

struct CleanRegistry {
    CleanRegistry() {
        Nx2GpuMemoryRegistry::Get().Reset();
    }
    ~CleanRegistry() {
        Nx2GpuMemoryRegistry::Get().Reset();
    }
};

} // Anonymous namespace

TEST_CASE("Nx2 sparse reservations sit under explicit overlays", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    std::vector<Event> events;
    Recorder recorder{events};

    registry.MapAt(0x104000, 0x900000, 0x1000);
    events.clear();
    REQUIRE(registry.CreateSparse(0x100000, 0x10000));
    CHECK(events ==
          std::vector<Event>{{'s', 0x100000, 0, 0x10000}, {'m', 0x104000, 0x900000, 0x1000}});

    events.clear();
    registry.Unmap(0x104000, 0x1000);
    CHECK(events == std::vector<Event>{{'v', 0x104000, 0, 0x1000}});

    registry.MapAt(0x10f000, 0x910000, 0x2000);
    events.clear();
    registry.Unmap(0x10f000, 0x2000);
    CHECK(events == std::vector<Event>{{'v', 0x10f000, 0, 0x1000}, {'u', 0x110000, 0, 0x1000}});
}

TEST_CASE("Nx2 sparse regions reject overlaps and mismatched destroys", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();

    REQUIRE(registry.CreateSparse(0x200000, 0x10000));
    CHECK_FALSE(registry.CreateSparse(0x200000, 0x10000));
    CHECK_FALSE(registry.CreateSparse(0x208000, 0x10000));
    CHECK_FALSE(registry.CreateSparse(0x1f8000, 0x9000));
    CHECK_FALSE(registry.CreateSparse(0x300000, 0));
    CHECK_FALSE(registry.DestroySparse(0x200000, 0x8000));
    CHECK_FALSE(registry.DestroySparse(0x204000, 0x10000));
    CHECK(registry.DestroySparse(0x200000, 0x10000));
    CHECK_FALSE(registry.DestroySparse(0x200000, 0x10000));

    const auto lease = registry.AcquireNative(0x300000, 0x1000);
    REQUIRE(lease);
    CHECK_FALSE(registry.CreateSparse(0x2ff000, 0x2000));
    REQUIRE(registry.CreateSparse(0x400000, 0x10000));
    CHECK_FALSE(registry.AcquireNative(0x40f000, 0x2000));
    registry.ReleaseNative(*lease);
}

TEST_CASE("Nx2 late listeners replay reservations, resident blocks, then overlays", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();

    REQUIRE(registry.CreateSparse(0x400000, 0x800000));
    registry.BackSparse(0x400000, 0xa00000, 0x400000);
    registry.BackSparse(0xc00000, 0xe00000, 0x1000);
    registry.BackSparse(0xbff000, 0xf00000, 0x2000);
    registry.MapAt(0x900000, 0xb00000, 0x1000);

    std::vector<Event> events;
    Recorder recorder{events};
    CHECK(events == std::vector<Event>{{'s', 0x400000, 0, 0x800000},
                                       {'b', 0x400000, 0xa00000, 0x400000},
                                       {'m', 0x900000, 0xb00000, 0x1000}});
}

TEST_CASE("Nx2 sparse destroys drop their overlays and resident blocks", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    std::vector<Event> events;
    Recorder recorder{events};

    REQUIRE(registry.CreateSparse(0x400000, 0x800000));
    registry.BackSparse(0x400000, 0xa00000, 0x400000);
    registry.MapAt(0x900000, 0xb00000, 0x1000);
    events.clear();
    REQUIRE(registry.DestroySparse(0x400000, 0x800000));
    CHECK(events == std::vector<Event>{{'d', 0x400000, 0, 0x800000}, {'u', 0x900000, 0, 0x1000}});

    std::vector<Event> late_events;
    Recorder late{late_events};
    CHECK(late_events.empty());
}

TEST_CASE("Nx2 retained backing completes only after its last user retires", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    std::vector<Event> events;
    Recorder recorder{events};
    const auto retired = [&events](u64 cpu, u64 size) {
        return [&events, cpu, size] { events.push_back({'c', 0, cpu, size}); };
    };

    registry.MapAt(0x600000, 0xd00000, 0x2000);
    events.clear();
    registry.ReleaseBacking(0xd00000, 0x2000, retired(0xd00000, 0x2000));
    CHECK(events.empty());
    registry.Unmap(0x600000, 0x2000);
    CHECK(events == std::vector<Event>{{'u', 0x600000, 0, 0x2000},
                                       {'r', 0, 0xd00000, 0x2000},
                                       {'c', 0, 0xd00000, 0x2000}});

    const auto lease = registry.AcquireNative(0xe00000, 0x1000);
    REQUIRE(lease);
    events.clear();
    registry.ReleaseBacking(0xe00000, 0x1000, retired(0xe00000, 0x1000));
    CHECK(events.empty());
    registry.ReleaseNative(*lease);
    CHECK(events == std::vector<Event>{{'u', 0xe00000, 0, 0x1000},
                                       {'r', 0, 0xe00000, 0x1000},
                                       {'c', 0, 0xe00000, 0x1000}});

    events.clear();
    registry.ReleaseBacking(0xf00000, 0x1000, retired(0xf00000, 0x1000));
    CHECK(events == std::vector<Event>{{'r', 0, 0xf00000, 0x1000}, {'c', 0, 0xf00000, 0x1000}});
}

TEST_CASE("Nx2 remapped GPU ranges replace their backing", "[nx2_mapping]") {
    CleanRegistry clean;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    std::vector<Event> events;
    Recorder recorder{events};

    registry.MapAt(0x700000, 0x1000000, 0x1000);
    events.clear();
    registry.MapAt(0x700000, 0x1100000, 0x1000);
    CHECK(events == std::vector<Event>{{'m', 0x700000, 0x1100000, 0x1000}});

    events.clear();
    registry.ReleaseBacking(0x1000000, 0x1000);
    CHECK(events == std::vector<Event>{{'r', 0, 0x1000000, 0x1000}});
    events.clear();
    registry.ReleaseBacking(0x1100000, 0x1000);
    CHECK(events.empty());
}
