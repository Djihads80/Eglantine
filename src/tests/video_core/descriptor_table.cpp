// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <catch2/catch_test_macros.hpp>
#include "video_core/texture_cache/descriptor_table.h"

namespace {
struct DescriptorMemory {
    std::array<std::array<u64, 128>, 2> banks{};

    // DescriptorTable::Read prefers a single-page host pointer when the memory manager offers one;
    // these tests are about the invalidation bookkeeping on the generic read path.
    const u8* GetPointer(GPUVAddr) const {
        return nullptr;
    }

    void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
        const auto bank = (address - 0x1000) / 0x1000;
        const auto offset = (address - 0x1000) % 0x1000;
        std::memcpy(output, reinterpret_cast<const u8*>(banks[bank].data()) + offset, size);
    }
};
} // namespace

TEST_CASE("Descriptor invalidation matches eager refresh across mutations", "[descriptor_table]") {
    DescriptorMemory memory;
    VideoCommon::DescriptorTable<u64> table;
    REQUIRE(table.Synchronize(0x1000, 127));
    std::array<bool, 128> seen{};
    std::array<u64, 128> previous{};
    std::mt19937 random{0x5080};
    unsigned bank{};
    u32 limit = 127;
    for (unsigned i = 0; i < 40000; ++i) {
        switch (random() % 5) {
        case 0:
            table.Invalidate();
            seen.fill(false);
            break;
        case 1:
            memory.banks[random() % 2][random() % 128] = random();
            break;
        case 2: {
            const auto next_bank = random() % 2;
            const u32 next_limit = random() % 2 ? 63 : 127;
            const bool changed = next_bank != bank || next_limit != limit;
            REQUIRE(table.Synchronize(0x1000 + next_bank * 0x1000, next_limit) == changed);
            if (changed) {
                seen.fill(false);
            }
            bank = next_bank;
            limit = next_limit;
            break;
        }
        default: {
            const u32 index = random() % (limit + 1);
            const u64 expected = memory.banks[bank][index];
            const bool changed = !seen[index] || previous[index] != expected;
            const auto actual = table.Read(memory, index);
            REQUIRE(actual.first == expected);
            REQUIRE(actual.second == changed);
            seen[index] = true;
            previous[index] = expected;
        }
        }
    }
}

TEST_CASE("Repeated image deletion cannot reuse stale descriptor view IDs", "[descriptor_table]") {
    DescriptorMemory memory;
    std::array<VideoCommon::DescriptorTable<u64>, 5> channels;
    std::array<std::array<unsigned, 128>, 5> view_ids{};
    unsigned live_generation = 1;
    const auto visit = [&](size_t channel, u32 index) {
        if (channels[channel].Read(memory, index).second) {
            view_ids[channel][index] = live_generation;
        }
        return view_ids[channel][index];
    };
    for (size_t channel = 0; channel < channels.size(); ++channel) {
        REQUIRE(channels[channel].Synchronize(0x1000, 127));
        for (u32 index = 0; index < 128; ++index) {
            REQUIRE(visit(channel, index) == live_generation);
        }
    }
    for (unsigned generation = 0; generation < 32; ++generation) {
        ++live_generation;
        for (unsigned deletion = 0; deletion < 50; ++deletion) {
            for (auto& table : channels) {
                table.Invalidate();
            }
        }
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            for (u32 index = 0; index < 128; ++index) {
                REQUIRE(visit(channel, index) == live_generation);
                REQUIRE_FALSE(channels[channel].Read(memory, index).second);
            }
        }
    }
}

TEST_CASE("Descriptor limits clear exposed slots after shrink and growth", "[descriptor_table]") {
    struct AddressMemory {
        const u8* GetPointer(GPUVAddr) const {
            return nullptr;
        }

        void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
            std::memcpy(output, &address, size);
        }
    } memory;
    constexpr std::array<u32, 9> limits{0, 63, 64, 127, 128, 0x7ffff, 0x80000, 0x80001, 0x100000};
    for (const u32 limit : limits) {
        VideoCommon::DescriptorTable<u64> table;
        REQUIRE(table.Synchronize(0x1000, limit));
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
        table.Refresh(0x1000, 0);
        table.Invalidate();
        CHECK(table.Read(memory, 0).second);
        CHECK_FALSE(table.Read(memory, 0).second);
        table.Refresh(0x1000, limit);
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
        table.Invalidate();
        table.Invalidate();
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
    }
    for (const u32 limit :
         {std::numeric_limits<u32>::max(), std::numeric_limits<u32>::max() - 0x1000}) {
        VideoCommon::DescriptorTable<u64> table;
        table.Refresh(0x1000, limit);
        CHECK(table.Read(memory, 0).second);
        table.Invalidate();
        CHECK(table.Read(memory, 0).second);
        CHECK_FALSE(table.Read(memory, 0).second);
    }
}

TEST_CASE("Descriptor invalidation CPU benchmark", "[.][cache_bench]") {
    DescriptorMemory memory;
    const auto measure = [&](bool deferred) {
        std::array<VideoCommon::DescriptorTable<u64>, 5> tables;
        for (auto& table : tables) {
            REQUIRE(table.Synchronize(0x1000, 127));
        }
        void (*volatile invalidate)(VideoCommon::DescriptorTable<u64>&) =
            deferred ? +[](VideoCommon::DescriptorTable<u64>& table) { table.Invalidate(); }
                     : +[](VideoCommon::DescriptorTable<u64>& table) {
                           std::ranges::fill(table.read_descriptors, 0);
                       };
        size_t changed{};
        const auto start = std::chrono::steady_clock::now();
        for (unsigned batch = 0; batch < 2000; ++batch) {
            for (unsigned deletion = 0; deletion < 50; ++deletion) {
                for (auto& table : tables) {
                    invalidate(table);
                }
            }
            for (auto& table : tables) {
                changed += table.Read(memory, 0).second;
            }
        }
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE(changed == 10000);
        return ms;
    };
    const double eager = measure(false);
    const double deferred = measure(true);
    std::cout << "Descriptor bitmap invalidation: eager=" << eager << " ms deferred=" << deferred
              << " ms (CPU only, not gameplay FPS)\n";
}
