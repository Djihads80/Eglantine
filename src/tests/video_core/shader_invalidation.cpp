// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/device_memory.h"
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
