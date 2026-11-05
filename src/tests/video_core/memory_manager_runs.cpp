// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <exception>
#include <limits>
#include <random>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/rasterizer_interface.h"

namespace {

class NullRasterizer final : public VideoCore::RasterizerInterface {
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
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
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
};

} // namespace

TEST_CASE("MaxContinuousRange from the mapping runs matches the page walk", "[memory_manager]") {
    REQUIRE(VideoCommon::Nx2Perf::Enabled());
    const u64 big_page_bits = GENERATE(16ULL, 17ULL);
    CAPTURE(big_page_bits);
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    NullRasterizer rasterizer;
    constexpr u64 address_space_bits = 32;
    constexpr GPUVAddr address_space_end = 1ULL << address_space_bits;
    Tegra::MemoryManager memory{system, device_memory, address_space_bits, 1ULL << 31,
                                big_page_bits};
    memory.BindRasterizer(&rasterizer);

    const u64 big_page = 1ULL << big_page_bits;
    constexpr u64 small_page = 0x1000;
    constexpr GPUVAddr window = 0x4000'0000;
    constexpr u64 window_size = 64ULL << 20;
    std::mt19937_64 rng{0x5eed'0000 + big_page_bits};
    const auto random = [&](u64 limit) { return std::uniform_int_distribution<u64>{0, limit - 1}(rng); };

    const auto check = [&](GPUVAddr address, size_t size) {
        CAPTURE(address, size);
        REQUIRE(memory.MaxContinuousRange(address, size) ==
                memory.MaxContinuousRangeUncached(address, size));
        REQUIRE(memory.GetMemoryLayoutSize(address) == memory.GetMemoryLayoutSizeUncached(address));
    };

    // Known answers first, so the comparison isn't between two zeros.
    memory.Map(window, 0x1000'0000, 4 * big_page, Tegra::PTEKind::PITCH, true);
    memory.Map(window + 4 * big_page, 0x1000'0000 + 4 * big_page, 3 * small_page,
               Tegra::PTEKind::PITCH, false);
    REQUIRE(memory.MaxContinuousRange(window + 0x10, 16ULL << 20) ==
            4 * big_page + 3 * small_page - 0x10);
    memory.Map(window + 2 * big_page, 0x2000'0000, big_page, Tegra::PTEKind::PITCH, true);
    REQUIRE(memory.MaxContinuousRange(window + 0x10, 16ULL << 20) == 2 * big_page - 0x10);
    REQUIRE(memory.MaxContinuousRange(window + 2 * big_page, 16ULL << 20) == big_page);
    memory.MapSparse(window + 3 * big_page, big_page, true);
    REQUIRE(memory.MaxContinuousRange(window + 2 * big_page, 16ULL << 20) == big_page);
    REQUIRE(memory.MaxContinuousRange(window + 3 * big_page, 16ULL << 20) == 0);
    memory.Unmap(window, 8 * big_page);
    REQUIRE(memory.MaxContinuousRange(window, 16ULL << 20) == 0);

    DAddr next_device = 0x1000'0000;
    u64 nonzero = 0;
    u64 queries = 0;
    for (int op = 0; op < 400; ++op) {
        const bool big = random(2) == 0;
        const u64 granule = big ? big_page : small_page;
        GPUVAddr address = window + random(window_size / granule) * granule;
        u64 size = (1 + random(64)) * granule;
        if (random(8) == 0) {
            // Unaligned to the page size the operation uses (mapped by whole pages anyway).
            address = window + random(window_size / small_page) * small_page;
            size = (1 + random(256)) * small_page;
        }
        size = (std::min)(size, window + window_size - address);
        const u32 kind = static_cast<u32>(random(4));
        const Tegra::PTEKind pte_kind = kind == 0 ? Tegra::PTEKind::GENERIC_16BX2
                                                  : Tegra::PTEKind::PITCH;
        switch (random(10)) {
        case 0:
        case 1:
        case 2:
            // Continues the last device range: merges with a neighbour mapped just before.
            memory.Map(address, next_device, size, pte_kind, big);
            next_device += size;
            break;
        case 3:
        case 4:
            next_device = 0x1000'0000 + random(0x4000'0000 / small_page) * small_page;
            memory.Map(address, next_device, size, pte_kind, big);
            next_device += size;
            break;
        case 5:
            // Remaps over the same GPU range with the device range shifted by a page.
            memory.Map(address, next_device + small_page, size, pte_kind, big);
            break;
        case 6:
            memory.MapSparse(address, size, big);
            break;
        default:
            memory.Unmap(address, (std::min)(size, 1 + random(16) * small_page + random(2) * 0x10));
            break;
        }
        for (int query = 0; query < 300; ++query) {
            const GPUVAddr query_address = window - 0x10000 + random(window_size + 0x20000);
            size_t query_size;
            switch (random(4)) {
            case 0:
                query_size = 1 + random(0x100);
                break;
            case 1:
                query_size = 1 + random(0x40000);
                break;
            case 2:
                query_size = 8ULL << 20;
                break;
            default:
                query_size = 1 + random(64ULL << 20);
                break;
            }
            check(query_address, query_size);
            nonzero += memory.MaxContinuousRangeUncached(query_address, query_size) != 0;
            ++queries;
        }
    }
    REQUIRE(nonzero > queries / 10);

    for (const auto& [address, size] : {
             std::pair<GPUVAddr, size_t>{0, 0},
             std::pair<GPUVAddr, size_t>{window, 0},
             std::pair<GPUVAddr, size_t>{address_space_end - 1, 1},
             std::pair<GPUVAddr, size_t>{address_space_end - 1, 2},
             std::pair<GPUVAddr, size_t>{address_space_end, 1},
             std::pair<GPUVAddr, size_t>{window, address_space_end},
             std::pair<GPUVAddr, size_t>{window, (std::numeric_limits<size_t>::max)()},
             std::pair<GPUVAddr, size_t>{(std::numeric_limits<u64>::max)(), 2}}) {
        check(address, size);
    }

    // Mappings at the top of the address space.
    memory.Map(address_space_end - 2 * big_page, 0x3000'0000, 2 * big_page, Tegra::PTEKind::PITCH,
               true);
    for (const u64 back : {u64{1}, u64{0x10}, big_page, 2 * big_page}) {
        check(address_space_end - back, back);
        check(address_space_end - back, back + 1);
    }
}
