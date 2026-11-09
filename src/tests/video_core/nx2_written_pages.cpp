// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <barrier>
#include <limits>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

namespace {

constexpr u64 PageSize = u64{1} << Tegra::Nx2WrittenPageBits;
constexpr u64 AddressLimit = u64{1} << 36;

struct WrittenPages {
    WrittenPages() { Tegra::Nx2ClearGpuWrittenPages(); }
    ~WrittenPages() { Tegra::Nx2ClearGpuWrittenPages(); }

    void MarkReference(u64 address, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 last = std::min<u64>((address + size - 1) >> Tegra::Nx2WrittenPageBits,
                                      expected.size() * 64 - 1);
        for (u64 page = address >> Tegra::Nx2WrittenPageBits; page <= last; ++page) {
            expected[page / 64] |= u64{1} << (page % 64);
        }
    }

    void Check() const {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (Tegra::g_nx2_gpu_written_pages[i].load(std::memory_order_relaxed) != expected[i]) {
                FAIL("GPU-written bitmap mismatch at word " << i);
            }
        }
        SUCCEED("GPU-written bitmap matches every reference word");
    }

    std::vector<u64> expected = std::vector<u64>(Tegra::g_nx2_gpu_written_pages.size());
};

} // namespace

TEST_CASE("GPU-written page marking preserves boundaries and wrapping arithmetic",
          "[video_core][nx2_written_pages]") {
    constexpr u64 Max = std::numeric_limits<u64>::max();
    const std::pair<u64, u64> ranges[]{
        {0, 0}, {0, 1}, {1, 1}, {PageSize - 1, 2},
        {0, PageSize}, {0, 63 * PageSize}, {0, 64 * PageSize}, {0, 65 * PageSize},
        {63 * PageSize + 1, PageSize}, {64 * PageSize - 1, 2},
        {61 * PageSize + 7, 130 * PageSize}, {AddressLimit - 1, 1},
        {AddressLimit - PageSize, PageSize * 3}, {AddressLimit, 1},
        {AddressLimit + PageSize, PageSize}, {Max, 0}, {Max, 1}, {Max, 2},
        {PageSize, Max}, {0, Max}, {1, Max}, {2, Max},
    };
    for (const auto [address, size] : ranges) {
        CAPTURE(address, size);
        WrittenPages pages;
        pages.MarkReference(address, size);
        Tegra::Nx2MarkGpuWritten(address, size);
        pages.Check();
        Tegra::Nx2MarkGpuWritten(address, size);
        pages.Check();
    }
}

TEST_CASE("GPU-written page marking matches overlapping and seeded random ranges",
          "[video_core][nx2_written_pages]") {
    WrittenPages pages;
    std::mt19937_64 random{0x62616e616e7a61};
    for (u32 i = 0; i < 1024; ++i) {
        const u64 address = i % 2 == 0 ? random() % AddressLimit : random() % (256 * PageSize);
        const u64 size = random() % (512 * PageSize);
        pages.MarkReference(address, size);
        Tegra::Nx2MarkGpuWritten(address, size);
    }
    pages.Check();
}

TEST_CASE("Concurrent overlapping GPU-written marks retain every writer's bits",
          "[video_core][nx2_written_pages]") {
    constexpr size_t Writers = 8;
    for (u64 round = 0; round < 64; ++round) {
        WrittenPages pages;
        std::barrier ready{static_cast<std::ptrdiff_t>(Writers + 1)};
        std::vector<std::jthread> writers;
        for (u64 writer = 0; writer < Writers; ++writer) {
            const u64 address = (round * 256 + writer * 17) * PageSize + 1;
            const u64 size = (writer + 1) * 8 * PageSize;
            pages.MarkReference(address, size);
            writers.emplace_back([&, address, size] {
                ready.arrive_and_wait();
                Tegra::Nx2MarkGpuWritten(address, size);
            });
        }
        ready.arrive_and_wait();
        writers.clear();
        pages.Check();
    }
}
