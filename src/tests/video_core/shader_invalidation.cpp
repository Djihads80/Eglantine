// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/device_memory.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/shader_cache.h"

namespace VideoCommon {
struct ShaderCacheTestAccess {
    static std::unique_ptr<ShaderCache> Create(Tegra::MaxwellDeviceMemoryManager& memory) {
        return std::unique_ptr<ShaderCache>{new ShaderCache{memory}};
    }
    static void Register(ShaderCache& cache, VAddr address, size_t size, u64 hash) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        cache.Register(std::move(data), address, size);
    }
    static const ShaderInfo* Find(const ShaderCache& cache, VAddr address) {
        return cache.TryGet(address);
    }
    static void RegisterNative(ShaderCache& cache, VAddr address, u64 hash, u64 key) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        auto lowered = std::make_shared<LoweredNativeCode>();
        lowered->native_program_key = key;
        data->lowered_native = std::move(lowered);
        cache.Register(std::move(data), address, 0x100);
    }
    static void Reject(ShaderCache& cache, VAddr address) {
        cache.rejected_native.emplace(address,
                                      std::make_exception_ptr(std::runtime_error{"rejected"}));
    }
    static bool IsRejected(ShaderCache& cache, VAddr address) {
        return cache.IsRejected(address);
    }
};
} // namespace VideoCommon

using VideoCommon::ShaderCacheTestAccess;

TEST_CASE("Shader batch invalidation matches sequential cache contents", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::mt19937 random{0x16'67};
    constexpr VAddr base = 0x100000;
    for (unsigned iteration = 0; iteration < 600; ++iteration) {
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                const auto size = 1 + random() % 65536;
                ShaderCacheTestAccess::Register(*batched, address, size, iteration + slot);
                ShaderCacheTestAccess::Register(*sequential, address, size, iteration + slot);
            }
        }
        std::vector<std::pair<DAddr, size_t>> writes;
        for (unsigned i = 0, count = random() % 24; i < count; ++i) {
            writes.emplace_back(base + random() % (128 * 4096), random() % 32768);
            if (i % 4 == 0) {
                writes.push_back(writes.back());
            }
        }
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        batched->InvalidateRegions(writes);
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            const auto actual = ShaderCacheTestAccess::Find(*batched, address);
            const auto expected = ShaderCacheTestAccess::Find(*sequential, address);
            REQUIRE(bool(actual) == bool(expected));
            if (actual) {
                REQUIRE(actual->unique_hash == expected->unique_hash);
            }
        }
    }
}

TEST_CASE("Shader empty batches preserve pending invalidations", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr address = 0x100000;
    ShaderCacheTestAccess::Register(*cache, address, 32768, 123);
    cache->OnCacheInvalidation(address, 1);
    cache->InvalidateRegions({});
    REQUIRE(ShaderCacheTestAccess::Find(*cache, address));
    const std::array unrelated{std::pair<DAddr, size_t>{address + 0x100000, 4096}};
    cache->InvalidateRegions(unrelated);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, address));
}

TEST_CASE("Debug server rejections drop matching native programs", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr by_key = 0x100000;
    constexpr VAddr by_hash = 0x200000;
    constexpr VAddr kept = 0x300000;
    constexpr VAddr maxwell = 0x400000;
    constexpr VAddr earlier = 0x500000;
    ShaderCacheTestAccess::RegisterNative(*cache, by_key, 0x11, 0xaaaa);
    ShaderCacheTestAccess::RegisterNative(*cache, by_hash, 0x22, 0xbbbb);
    ShaderCacheTestAccess::RegisterNative(*cache, kept, 0x33, 0xcccc);
    ShaderCacheTestAccess::Register(*cache, maxwell, 0x100, 0xaaaa);
    ShaderCacheTestAccess::Reject(*cache, earlier);

    VideoCore::Nx2Debug::BeginSession();
    const std::array<u64, 2> rejected{0xaaaa, 0x22};
    const u64 generation = VideoCore::Nx2Debug::SetRejected(rejected, true);
    const auto change = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(change);
    REQUIRE(change->generation == generation);
    REQUIRE_FALSE(change->unrejected);
    cache->RefreshDebugRejections(change->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_key));
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_hash));
    REQUIRE(ShaderCacheTestAccess::Find(*cache, kept));
    // Only lowered native programs are matched; a Maxwell program with the same hash stays.
    REQUIRE(ShaderCacheTestAccess::Find(*cache, maxwell));
    REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, earlier));

    const std::array<u64, 1> restored{0xaaaa};
    VideoCore::Nx2Debug::SetRejected(restored, false);
    const auto undo = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(undo);
    REQUIRE(undo->unrejected);
    cache->RefreshDebugRejections(undo->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::IsRejected(*cache, earlier));
    REQUIRE(VideoCore::Nx2Debug::RejectedHashes() == std::vector<u64>{0x22});
    VideoCore::Nx2Debug::EndSession();
}

TEST_CASE("Shader invalidation CPU benchmark", "[.][cache_bench]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::vector<std::pair<DAddr, size_t>> writes;
    constexpr VAddr base = 0x100000;
    for (unsigned slot = 0; slot < 768; ++slot) {
        writes.emplace_back(base + slot * 65536, 1);
    }
    double sequential_ms{}, batched_ms{};
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        for (unsigned slot = 0; slot < 1024; ++slot) {
            const auto address = base + slot * 65536;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                ShaderCacheTestAccess::Register(*batched, address, 64, slot);
                ShaderCacheTestAccess::Register(*sequential, address, 64, slot);
            }
        }
        auto start = std::chrono::steady_clock::now();
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        sequential_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        start = std::chrono::steady_clock::now();
        batched->InvalidateRegions(writes);
        batched_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE_FALSE(ShaderCacheTestAccess::Find(*batched, base));
        REQUIRE(ShaderCacheTestAccess::Find(*batched, base + 1023 * 65536));
    }
    std::cout << "Shader invalidation: sequential=" << sequential_ms << " ms batched=" << batched_ms
              << " ms (CPU only, not gameplay FPS)\n";
}
