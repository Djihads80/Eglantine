// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/list_copy_demand.h"

TEST_CASE("Command-page eager copy demand learns zero and saturates conservatively",
          "[list_copy_demand]") {
    VideoCommon::ListCopyDemand demand;
    CHECK_FALSE(demand.ShouldCopy());
    CHECK(demand.Request()); // Zero is a valid DMA/inline writer identity.
    CHECK(demand.ShouldCopy());
    CHECK_FALSE(demand.Request());
    for (u64 writer = 1; writer < 5; ++writer) {
        demand.NoteWrite(writer);
        CHECK_FALSE(demand.ShouldCopy());
        CHECK(demand.Request());
        CHECK(demand.ShouldCopy());
    }
    demand.NoteWrite(0x123456);
    CHECK(demand.ShouldCopy()); // More writer identities than fit means copy every writer.
    CHECK_FALSE(demand.Request());
}

TEST_CASE("Command-page demand misses preserve versioned fallback in a page model",
          "[list_copy_demand]") {
    // This covers policy transitions; the real read/drain/flush path is checked by lab runs.
    VideoCommon::ListCopyDemand demand;
    std::mt19937 rng{0x762};
    u64 version = 0;
    u64 copy_version = ~u64{0};
    u64 gpu_word = 0, copy_word = 0, guest_word = 0;
    u64 fallbacks = 0, skipped = 0;
    for (u64 step = 0; step < 10000; ++step) {
        if (rng() % 4 != 0) {
            const u64 writer = rng() % 4;
            demand.NoteWrite(writer);
            gpu_word = rng();
            ++version;
            if (demand.ShouldCopy()) {
                copy_version = version;
                copy_word = gpu_word;
            } else {
                ++skipped;
            }
        } else {
            demand.Request();
            u64 read_word;
            if (copy_version == version) {
                read_word = copy_word;
            } else {
                ++fallbacks;
                guest_word = gpu_word;
                read_word = guest_word;
            }
            CHECK(read_word == gpu_word);
        }
    }
    CHECK(skipped > 0);
    CHECK(fallbacks > 0);
}
