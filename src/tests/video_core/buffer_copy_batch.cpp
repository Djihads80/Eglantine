// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_copy_batch.h"

namespace {

using VideoCommon::BufferCopy;
using VideoCommon::ForEachBufferCopyBatch;
using Entry = std::pair<BufferCopy, u32>;

TEST_CASE("Buffer download batches preserve source order, offsets and copied bytes",
          "[video_core][readback][copy_batch]") {
    std::array<std::array<u8, 64>, 2> sources{};
    for (size_t source = 0; source < sources.size(); ++source) {
        for (size_t offset = 0; offset < sources[source].size(); ++offset) {
            sources[source][offset] = static_cast<u8>(source * 128 + offset);
        }
    }
    const std::array<Entry, 5> entries{{
        {{0, 16, 4}, 0},
        {{8, 24, 8}, 0},
        {{4, 40, 4}, 1},
        {{12, 0, 4}, 0},
        {{16, 4, 8}, 0},
    }};
    std::array<u8, 64> output;
    output.fill(0xcd);
    std::vector<std::pair<u32, size_t>> batches;
    std::vector<Entry> emitted;
    const size_t commands = ForEachBufferCopyBatch<u32>(
        entries, [&](u32 source, std::span<const BufferCopy> copies) {
            batches.emplace_back(source, copies.size());
            for (const auto& copy : copies) {
                emitted.emplace_back(copy, source);
                REQUIRE(copy.src_offset + copy.size <= sources.at(source).size());
                REQUIRE(copy.dst_offset + copy.size <= output.size());
                std::copy_n(sources[source].begin() + copy.src_offset, copy.size,
                            output.begin() + copy.dst_offset);
            }
        });
    CHECK(commands == 3);
    CHECK(batches == std::vector<std::pair<u32, size_t>>{{0, 2}, {1, 1}, {0, 2}});
    REQUIRE(emitted.size() == entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        CHECK(emitted[index].second == entries[index].second);
        CHECK(emitted[index].first.src_offset == entries[index].first.src_offset);
        CHECK(emitted[index].first.dst_offset == entries[index].first.dst_offset);
        CHECK(emitted[index].first.size == entries[index].first.size);
    }
    std::array<u8, 64> expected;
    expected.fill(0xcd);
    for (size_t index = 0; index < 12; ++index) expected[index] = static_cast<u8>(12 + index);
    for (size_t index = 0; index < 4; ++index) expected[16 + index] = static_cast<u8>(index);
    for (size_t index = 0; index < 8; ++index) expected[24 + index] = static_cast<u8>(8 + index);
    for (size_t index = 0; index < 4; ++index) expected[40 + index] = static_cast<u8>(132 + index);
    CHECK(output == expected);
}

TEST_CASE("Buffer download batches handle empty input and retain 64-bit offsets",
          "[video_core][readback][copy_batch]") {
    size_t calls = 0;
    CHECK(ForEachBufferCopyBatch<u32>({}, [&](u32, std::span<const BufferCopy>) { ++calls; }) == 0);
    CHECK(calls == 0);
    constexpr u64 large_offset = 1ULL << 34;
    const std::array<Entry, 1> entries{{{{large_offset, large_offset + 4096, 16}, 7}}};
    CHECK(ForEachBufferCopyBatch<u32>(entries, [&](u32 source, std::span<const BufferCopy> copies) {
        ++calls;
        CHECK(source == 7);
        REQUIRE(copies.size() == 1);
        CHECK(copies.front().src_offset == large_offset);
        CHECK(copies.front().dst_offset == large_offset + 4096);
        CHECK(copies.front().size == 16);
    }) == 1);
    CHECK(calls == 1);
}

} // namespace
