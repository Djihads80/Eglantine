// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_dependency_trace.h"

namespace {
using Trace = VideoCommon::BufferDependencyTrace;
using Producer = Trace::Producer;
using Input = Trace::Input;
constexpr DAddr Base = 0x1000;
constexpr u64 Consumer = 0xabc;
} // namespace

TEST_CASE("Buffer dependency trace splits overlapping declared writes and unknown gaps",
          "[buffer_dependency_trace]") {
    Trace trace;
    trace.Write(Base + 4, 12, {Producer::Compute, 0x123, 7});
    trace.Write(Base + 8, 4, {Producer::Graphics, 0, 8});
    trace.BeginDispatch(Consumer);
    trace.ReadDirty(Base, 20, Input::Storage, 8, true);
    trace.ReadClean(5, Input::Uniform, false);
    trace.CommitWriter();
    const auto& edges = trace.Edges();
    CHECK(edges.at({Consumer, Input::Storage, Producer::Compute, 0x123}).bytes == 8);
    CHECK(edges.at({Consumer, Input::Storage, Producer::Compute, 0x123}).recent_bytes == 0);
    CHECK(edges.at({Consumer, Input::Storage, Producer::Compute, 0x123}).cpu_dirty_observations ==
          2);
    CHECK(edges.at({Consumer, Input::Storage, Producer::Graphics, 0}).bytes == 4);
    CHECK(edges.at({Consumer, Input::Storage, Producer::Graphics, 0}).recent_bytes == 4);
    CHECK(edges.at({Consumer, Input::Storage, Producer::Unknown, 0}).bytes == 8);
    CHECK(edges.at({Consumer, Input::Uniform, Producer::Clean, 0}).bytes == 5);
    trace.EndDispatch();
    CHECK(trace.ActiveShader() == 0);
    trace.BeginDispatch(0xdef);
    trace.ReadDirty(Base, 20, Input::Storage, 8, false);
    trace.EndDispatch();
    CHECK(trace.Edges().size() == 4); // A non-writer dispatch never commits its input observations.
    trace.Forget(Base + 5, 2);
    trace.BeginDispatch(0xdef);
    trace.ReadDirty(Base + 4, 4, Input::Storage, 8, false);
    trace.CommitWriter();
    CHECK(trace.Edges().at({0xdef, Input::Storage, Producer::Unknown, 0}).bytes == 2);
    CHECK(trace.Edges().at({0xdef, Input::Storage, Producer::Compute, 0x123}).bytes == 2);
}

TEST_CASE("Buffer dependency trace capacity loss stays unknown and keeps exact bounds",
          "[buffer_dependency_trace]") {
    Trace trace{2, 1};
    trace.Write(Base, 20, {Producer::Compute, 1, 1});
    trace.Write(Base + 8, 4, {Producer::Other, 0, 2});
    CHECK(trace.RangeCount() <= 2);
    CHECK(trace.Resets() == 1);
    trace.BeginDispatch(Consumer);
    trace.ReadDirty(Base, 4, Input::Storage, 2, false);
    trace.CommitWriter();
    CHECK(trace.Edges().at({Consumer, Input::Storage, Producer::Unknown, 0}).bytes == 4);
    trace.ReadDirty(Base + 8, 4, Input::Storage, 2, false);
    trace.CommitWriter();
    CHECK(trace.Edges().size() == 1);
    CHECK(trace.Drops() == 1);
    trace.Write(Base, 0, {});
    trace.Write(~DAddr{0} - 1, 2, {});
    CHECK(trace.Drops() == 3);
    Trace empty{0, 0};
    empty.Write(Base, 1, {});
    empty.BeginDispatch(Consumer);
    empty.ReadDirty(Base, 2, Input::Indirect, 0, false);
    empty.CommitWriter();
    CHECK(empty.RangeCount() == 0);
    CHECK(empty.Edges().empty());
    CHECK(empty.Drops() == 2);
}

TEST_CASE("Buffer dependency trace agrees with an independent byte origin oracle",
          "[buffer_dependency_trace]") {
    Trace trace;
    std::array<Trace::Origin, 128> oracle{};
    std::map<Trace::Edge, Trace::Stats> expected;
    std::mt19937 rng{0x302176};
    for (u64 step = 0; step < 500; ++step) {
        const size_t begin = rng() % oracle.size();
        const size_t size = 1 + rng() % (oracle.size() - begin);
        const u64 frame = step / 5;
        const Trace::Origin origin{static_cast<Producer>(1 + rng() % 3), rng() % 3, frame};
        trace.Write(Base + begin, size, origin);
        std::fill_n(oracle.begin() + begin, size, origin);
        if (step % 7 == 0) {
            const size_t forget_begin = rng() % oracle.size();
            const size_t forget_size = 1 + rng() % (oracle.size() - forget_begin);
            trace.Forget(Base + forget_begin, forget_size);
            std::fill_n(oracle.begin() + forget_begin, forget_size, Trace::Origin{});
        }

        const size_t read_begin = rng() % oracle.size();
        const size_t read_size = 1 + rng() % (oracle.size() - read_begin);
        const auto input = static_cast<Input>(rng() % 4);
        trace.BeginDispatch(Consumer);
        trace.ReadDirty(Base + read_begin, read_size, input, frame, false);
        trace.CommitWriter();
        for (size_t offset = read_begin; offset < read_begin + read_size; ++offset) {
            const auto& byte = oracle[offset];
            auto& stats = expected[{Consumer, input, byte.kind, byte.shader}];
            ++stats.bytes;
            stats.recent_bytes += byte.kind != Producer::Unknown && byte.frame == frame;
        }
        REQUIRE(trace.Edges().size() == expected.size());
        for (const auto& [edge, stats] : expected) {
            CHECK(trace.Edges().at(edge).bytes == stats.bytes);
            CHECK(trace.Edges().at(edge).recent_bytes == stats.recent_bytes);
        }
        trace.EndDispatch();
    }
    CHECK(trace.Drops() == 0);
    CHECK(trace.Resets() == 0);
}
