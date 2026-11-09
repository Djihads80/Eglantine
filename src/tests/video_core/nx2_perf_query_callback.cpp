// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"

namespace {

class QueryRasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& operation, bool) override {
        ++fences;
        if (defer) {
            pending.push_back(std::move(operation));
        } else {
            operation();
        }
    }
    void SyncOperation(std::function<void()>&& operation) override {
        pending.push_back(std::move(operation));
    }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    bool defer{};
    u32 fences{};
    std::vector<std::function<void()>> pending;
};

class PayloadStreamer final : public VideoCommon::StreamerInterface {
public:
    PayloadStreamer() : StreamerInterface{0} {
        amend_value = 7;
    }

    size_t WriteCounter(VAddr address, bool timestamp, u32 value,
                        std::optional<u32>) override {
        query = VideoCommon::GuestQuery{timestamp, address, value};
        query.flags |= extra_flags;
        return 0;
    }
    VideoCommon::QueryBase* GetQuery(size_t) override {
        return &query;
    }
    void Free(size_t) override {
        ++freed;
    }
    u64 Accumulation() const {
        return accumulation_value;
    }

    VideoCommon::GuestQuery query{false, 0, 0};
    VideoCommon::QueryFlagBits extra_flags{};
    u32 freed{};
};

struct Runtime {
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType type) {
        return type == VideoCommon::QueryType::Payload ? &streamer : nullptr;
    }
    void Bind3DEngine(Tegra::Engines::Maxwell3D*) {}

    template <typename T>
    void SyncValues(std::span<T> values) {
        synced.assign(values.begin(), values.end());
        ++syncs;
    }

    PayloadStreamer streamer;
    std::vector<VideoCommon::SyncValuesStruct> synced;
    u32 syncs{};
};

struct Traits {
    using RuntimeType = Runtime;
};

class QueryCache final : public VideoCommon::QueryCacheBase<Traits> {
public:
    using QueryCacheBase::QueryCacheBase;
    using QueryCacheBase::UnregisterPending;

    void UseMemory(Tegra::MemoryManager& memory) {
        gpu_memory = &memory;
    }
};

} // namespace

TEST_CASE("Nx2Perf compute: reused payload upload scratch preserves query filtering",
          "[video_core][nx2_perf][compute_path]") {
    Runtime runtime;
    VideoCommon::GuestStreamer<Traits> streamer{0, runtime};
    VideoCommon::StreamerInterface& interface = streamer;
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        std::vector<size_t> ids;
        std::vector<VideoCommon::SyncValuesStruct> expected;
        const u32 count = iteration % 13;
        for (u32 index = 0; index < count; ++index) {
            const u64 address = 0x1000 + index * 16;
            const u32 value = iteration * 100 + index;
            const bool timestamp = (index & 1) != 0;
            const size_t id = interface.WriteCounter(address, timestamp, value);
            ids.push_back(id);
            if (index % 4 == 0) {
                interface.GetQuery(id)->flags |= VideoCommon::QueryFlagBits::IsInvalidated;
            } else if (index % 4 == 1) {
                interface.GetQuery(id)->flags |= VideoCommon::QueryFlagBits::IsRewritten;
            } else {
                expected.push_back({address, value, timestamp ? 8U : 4U});
            }
        }
        REQUIRE(interface.HasPendingSync() == (count != 0));
        const u32 syncs = runtime.syncs;
        streamer.SyncWrites();
        REQUIRE_FALSE(interface.HasPendingSync());
        REQUIRE(runtime.syncs == syncs + (expected.empty() ? 0U : 1U));
        if (!expected.empty()) {
            REQUIRE(runtime.synced.size() == expected.size());
            for (size_t index = 0; index < expected.size(); ++index) {
                REQUIRE(runtime.synced[index].address == expected[index].address);
                REQUIRE(runtime.synced[index].value == expected[index].value);
                REQUIRE(runtime.synced[index].size == expected[index].size);
            }
        }
        for (size_t index = 0; index < ids.size(); ++index) {
            REQUIRE(True(interface.GetQuery(ids[index])->flags &
                         VideoCommon::QueryFlagBits::IsHostSynced) == (index % 4 >= 2));
            interface.Free(ids[index]);
        }
        streamer.SyncWrites();
        REQUIRE(runtime.syncs == syncs + (expected.empty() ? 0U : 1U));
    }
}

TEST_CASE("Nx2Perf compute: query callbacks preserve immediate and deferred writes",
          "[video_core][nx2_perf][compute_path]") {
    using VideoCommon::QueryFlagBits;
    using VideoCommon::QueryPropertiesFlags;
    using VideoCommon::QueryType;
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    u8* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(page) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    Tegra::MaxwellDeviceMemoryManager device_memory{system.DeviceMemory()};
    const auto asid = device_memory.RegisterProcess(&process.GetMemory());
    device_memory.Map(0x1000, 0x4000, 0x1000, asid);
    QueryRasterizer rasterizer;
    device_memory.BindInterface(&rasterizer);
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    memory.Map(0x10000, 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    Tegra::GPU gpu{system, false, false};
    Runtime runtime;
    QueryCache cache{gpu, rasterizer, device_memory, runtime};
    cache.UseMemory(memory);

    const auto old_behavior = Settings::values.gpu_fence_behavior.GetValue();
    const auto old_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.gpu_fence_behavior.SetValue(old_behavior);
        Settings::values.current_gpu_accuracy = old_accuracy;
    };
    for (const auto accuracy : {Settings::GpuAccuracy::Low, Settings::GpuAccuracy::High}) {
        Settings::values.current_gpu_accuracy = accuracy;
        for (const auto behavior : {Settings::GpuFenceBehavior::Default,
                                    Settings::GpuFenceBehavior::Immediate,
                                    Settings::GpuFenceBehavior::Balanced,
                                    Settings::GpuFenceBehavior::Accurate}) {
            Settings::values.gpu_fence_behavior.SetValue(behavior);
            rasterizer.defer = Settings::IsGPUFenceBehaviorDefault()
                                   ? Settings::IsGPULevelHigh()
                                   : Settings::IsGPUFenceBehaviorBalanced() ||
                                         Settings::IsGPUFenceBehaviorAccurate();
            for (const bool invalidated : {false, true}) {
                for (u32 payload : {0U, 1U, 0xfffffff9U, 0xffffffffU}) {
                    CAPTURE(accuracy, behavior, invalidated, payload);
                    std::memset(page, 0x5a, 16);
                    runtime.streamer.extra_flags = invalidated ? QueryFlagBits::IsInvalidated
                                                              : QueryFlagBits{};
                    const u32 freed_before = runtime.streamer.freed;
                    const u32 fences_before = rasterizer.fences;
                    cache.CounterReport(0x10000, QueryType::Payload,
                                        QueryPropertiesFlags::IsAFence, payload, 0);
                    REQUIRE(rasterizer.fences == fences_before + 1);
                    if (rasterizer.defer) {
                        REQUIRE(device_memory.Read<u32>(0x1000) == 0x5a5a5a5aU);
                        REQUIRE(runtime.streamer.freed == freed_before);
                        REQUIRE(rasterizer.pending.size() == 1);
                        rasterizer.pending.front()();
                        rasterizer.pending.clear();
                        cache.UnregisterPending();
                    } else {
                        REQUIRE(rasterizer.pending.empty());
                    }
                    REQUIRE(runtime.streamer.freed == freed_before + 1);
                    REQUIRE(device_memory.Read<u32>(0x1000) ==
                            (invalidated ? 0x5a5a5a5aU : payload + 7U));
                    REQUIRE(device_memory.Read<u32>(0x1004) == 0x5a5a5a5aU);
                    if (!invalidated) {
                        REQUIRE(runtime.streamer.Accumulation() == u64{payload} + 7);
                    }
                }
            }
        }
    }
}
