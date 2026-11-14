// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/list_writer_trace.h"

namespace {
using Trace = VideoCommon::ListWriterTrace;
constexpr DAddr Page = 0x1000;
constexpr u64 Shader = 0x123;

void Capture(Trace& trace, uintptr_t key, u64 previous, std::span<const u8> bytes,
             size_t read_begin = 0, size_t read_size = 0, bool recent = false) {
    trace.BeginDispatch(Shader);
    trace.NoteGpuWrite(Page, previous, true);
    trace.SetRecentInput(recent);
    trace.RegisterCopy(key, Page, previous + 1, 0, static_cast<u32>(bytes.size()));
    trace.NoteFrontRead(key, read_begin, read_size);
    trace.EndDispatch();
    trace.FreezeCopy(key, previous + 1);
    trace.CompleteCopy(key, bytes);
}
} // namespace

TEST_CASE("List writer trace distinguishes changed command bytes from other page changes",
          "[list_writer_trace]") {
    Trace trace;
    std::array<u8, 16> bytes{};
    Capture(trace, 1, 0, bytes);
    Capture(trace, 2, 1, bytes, 0, 4);
    bytes[12] = 1;
    Capture(trace, 3, 2, bytes, 0, 4);
    bytes[2] = 2;
    Capture(trace, 4, 3, bytes, 0, 4, true);
    const auto& s = trace.ShaderStats().at(Shader);
    CHECK(s.completed == u64{4});
    CHECK(s.baselines == u64{1});
    CHECK(s.unchanged == u64{1});
    CHECK(s.changed == u64{2});
    CHECK(s.adjacent_changed == u64{2});
    CHECK(s.read_copies == u64{3});
    CHECK(s.read_changed == u64{1});
    CHECK(s.adjacent_read_changed == u64{1});
    CHECK(s.recent_read_changed == u64{1});
}

TEST_CASE("List writer trace excludes intervening writers and host-mutated staging copies",
          "[list_writer_trace]") {
    Trace trace;
    std::array<u8, 16> bytes{};
    Capture(trace, 1, 0, bytes);
    trace.NoteHostWrite(Page);
    bytes[1] = 1;
    Capture(trace, 2, 2, bytes, 0, 4);
    trace.BeginDispatch(Shader);
    trace.NoteGpuWrite(Page, 3, true);
    trace.RegisterCopy(3, Page, 4, 0, 16);
    trace.NoteFrontRead(3, 0, 4);
    trace.EndDispatch();
    bytes[1] = 2;
    trace.FreezeCopy(3, 5);
    trace.CompleteCopy(3, bytes);
    bytes[1] = 3;
    Capture(trace, 4, 5, bytes, 0, 4);
    const auto& s = trace.ShaderStats().at(Shader);
    CHECK(s.mixed_changed == u64{2});
    CHECK(s.adjacent_read_changed == u64{1});
    CHECK(s.read_changed == u64{2});
}

TEST_CASE("List writer trace handles delayed completion, partial ranges and bounded storage",
          "[list_writer_trace]") {
    Trace trace;
    std::array<u8, 16> bytes{};
    Capture(trace, 1, 0, bytes);
    for (u64 serial : {u64{1}, u64{2}}) {
        trace.BeginDispatch(Shader);
        trace.NoteGpuWrite(Page, serial, true);
        trace.RegisterCopy(static_cast<uintptr_t>(serial + 1), Page, serial + 1, 0, 16);
        trace.EndDispatch();
    }
    bytes[0] = 3;
    trace.CompleteCopy(3, bytes);
    bytes[0] = 2;
    trace.CompleteCopy(2, bytes);
    bytes[0] = 4;
    Capture(trace, 4, 3, bytes, 0, 1);
    CHECK(trace.ShaderStats().at(Shader).out_of_order == u64{1});
    CHECK(trace.ShaderStats().at(Shader).adjacent_read_changed == u64{1});
    trace.BeginDispatch(Shader);
    trace.NoteGpuWrite(Page, 4, true);
    trace.RegisterCopy(5, Page, 5, 4, 16);
    trace.EndDispatch();
    trace.CompleteCopy(5, bytes);
    CHECK(trace.ShaderStats().at(Shader).incomparable == u64{1});

    Trace bounded{Trace::Limits{1, 1, 1, 16}};
    bounded.BeginDispatch(Shader);
    bounded.NoteGpuWrite(Page, 0, true);
    bounded.RegisterCopy(1, Page, 1, 0, 16);
    bounded.RegisterCopy(2, Page, 1, 0, 16);
    bounded.NoteGpuWrite(Page + 4096, 0, true);
    CHECK(bounded.PageCount() == size_t{1});
    CHECK(bounded.PendingCount() == size_t{1});
    bounded.CompleteCopy(1, std::span<const u8>{bytes}.first(8));
    CHECK(bounded.Dropped() == u64{3});
    CHECK(bounded.PendingCount() == size_t{0});
    bounded.RegisterCopy(3, Page, 1, 0, 4097);
    CHECK(bounded.Dropped() == u64{4});
}

TEST_CASE("List writer trace matches a randomized byte-read and version oracle",
          "[list_writer_trace]") {
    Trace trace;
    std::array<u8, 64> current{}, previous{};
    std::mt19937 random{0xc011157};
    u64 version{}, expected_changed{}, expected_read_changed{}, expected_adjacent_read{};
    for (size_t step = 0; step < 500; ++step) {
        const bool gap = (random() % 5) == 0;
        const bool host_shadow = (random() % 7) == 0;
        if (gap) {
            trace.NoteHostWrite(Page);
            ++version;
            current[random() % current.size()] ^= 1;
        }
        trace.BeginDispatch(Shader);
        trace.NoteGpuWrite(Page, version, true);
        ++version;
        current[random() % current.size()] ^= static_cast<u8>(random() % 256);
        trace.RegisterCopy(step + 1, Page, version, 0, static_cast<u32>(current.size()));
        const size_t begin = random() % current.size();
        const size_t size = 1 + random() % (current.size() - begin);
        trace.NoteFrontRead(step + 1, begin, size);
        trace.EndDispatch();
        if (host_shadow) {
            ++version;
            current[random() % current.size()] ^= 1;
        }
        trace.FreezeCopy(step + 1, version);
        trace.CompleteCopy(step + 1, current);
        if (step != 0) {
            bool changed = false, read_changed = false;
            for (size_t index = 0; index < current.size(); ++index) {
                changed |= current[index] != previous[index];
                read_changed |=
                    current[index] != previous[index] && index >= begin && index < begin + size;
            }
            expected_changed += changed;
            expected_read_changed += read_changed && !host_shadow;
            expected_adjacent_read += read_changed && !host_shadow && !gap;
        }
        previous = current;
        const auto& s = trace.ShaderStats().at(Shader);
        CHECK(s.changed == expected_changed);
        CHECK(s.read_changed == expected_read_changed);
        CHECK(s.adjacent_read_changed == expected_adjacent_read);
    }
}
