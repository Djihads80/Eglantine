// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

#include "common/common_types.h"

namespace VideoCommon {

// Declared buffer-range observations, not shader byte-access or complete alias provenance.
// The buffer-cache mutex protects this diagnostic; it never changes synchronization.
class BufferDependencyTrace {
public:
    enum class Producer : u32 { Unknown, Compute, Graphics, Other, Clean };
    enum class Input : u32 { Storage, Uniform, Texture, Indirect };
    struct Origin {
        Producer kind{};
        u64 shader{}, frame{};
        bool operator==(const Origin&) const = default;
    };
    struct Edge {
        u64 consumer{};
        Input input{};
        Producer producer{};
        u64 shader{};
        auto Key() const {
            return std::tuple{consumer, input, producer, shader};
        }
        bool operator<(const Edge& other) const {
            return Key() < other.Key();
        }
    };
    struct Stats {
        u64 observations{}, bytes{}, recent_bytes{}, cpu_dirty_observations{};
    };

    explicit BufferDependencyTrace(size_t ranges_ = 65536, size_t edges_ = 4096)
        : max_ranges{ranges_}, max_edges{edges_} {}

    void BeginDispatch(u64 shader) {
        active_shader = shader;
        pending.clear();
    }
    void EndDispatch() {
        active_shader = 0;
        pending.clear();
    }
    [[nodiscard]] u64 ActiveShader() const {
        return active_shader;
    }

    void Write(DAddr begin, u64 size, Origin origin) {
        if (!Valid(begin, size)) {
            ++drops;
            return;
        }
        const DAddr end = begin + size;
        Split(begin);
        Split(end);
        ranges.erase(ranges.lower_bound(begin), ranges.lower_bound(end));
        if (ranges.size() >= max_ranges) {
            // Forgetting history must produce unknown observations, never independence.
            ranges.clear();
            ++resets;
        }
        if (max_ranges == 0) {
            ++drops;
            return;
        }
        ranges.emplace(begin, Range{end, origin});
        // Splits can temporarily add two entries. Keep the persistent bound exact.
        if (ranges.size() > max_ranges) {
            ranges.clear();
            ranges.emplace(begin, Range{end, origin});
            ++resets;
        }
    }
    void Forget(DAddr begin, u64 size) {
        if (!Valid(begin, size)) {
            return;
        }
        Split(begin);
        Split(begin + size);
        ranges.erase(ranges.lower_bound(begin), ranges.lower_bound(begin + size));
        if (ranges.size() > max_ranges) {
            ranges.clear();
            ++resets;
        }
    }

    // Call once for each exact GPU-dirty subrange, before this dispatch marks any writes.
    void ReadDirty(DAddr begin, u64 size, Input input, u64 frame, bool cpu_dirty) {
        if (!active_shader || !Valid(begin, size)) {
            return;
        }
        const DAddr end = begin + size;
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) {
            --it;
        }
        DAddr cursor = begin;
        for (; it != ranges.end() && it->first < end; ++it) {
            const auto& [start, range] = *it;
            if (range.end <= cursor) {
                continue;
            }
            if (start > cursor) {
                Observe(input, Producer::Unknown, 0, start - cursor, false, cpu_dirty);
                cursor = start;
            }
            const DAddr stop = (std::min)(range.end, end);
            Observe(input, range.origin.kind, range.origin.shader, stop - cursor,
                    range.origin.frame == frame, cpu_dirty);
            cursor = stop;
        }
        if (cursor < end) {
            Observe(input, Producer::Unknown, 0, end - cursor, false, cpu_dirty);
        }
    }
    void ReadClean(u64 size, Input input, bool cpu_dirty) {
        if (active_shader && size) {
            Observe(input, Producer::Clean, 0, size, false, cpu_dirty);
        }
    }
    void CommitWriter() {
        for (const auto& [edge, stats] : pending) {
            if (!edges.contains(edge) && edges.size() >= max_edges) {
                ++drops;
                continue;
            }
            Add(edges[edge], stats);
        }
        pending.clear();
    }

    const auto& Edges() const {
        return edges;
    }
    size_t RangeCount() const {
        return ranges.size();
    }
    u64 Drops() const {
        return drops;
    }
    u64 Resets() const {
        return resets;
    }

private:
    struct Range {
        DAddr end;
        Origin origin;
    };
    static bool Valid(DAddr begin, u64 size) {
        return size != 0 && size <= (std::numeric_limits<DAddr>::max)() - begin;
    }
    void Split(DAddr at) {
        auto it = ranges.upper_bound(at);
        if (it == ranges.begin()) {
            return;
        }
        --it;
        if (it->first < at && at < it->second.end) {
            const Range tail = it->second;
            it->second.end = at;
            ranges.emplace(at, tail);
        }
    }
    static void Add(Stats& dest, const Stats& src) {
        dest.observations += src.observations;
        dest.bytes += src.bytes;
        dest.recent_bytes += src.recent_bytes;
        dest.cpu_dirty_observations += src.cpu_dirty_observations;
    }
    void Observe(Input input, Producer kind, u64 shader, u64 bytes, bool recent, bool cpu_dirty) {
        const Edge edge{active_shader, input, kind, shader};
        if (!pending.contains(edge) && pending.size() >= max_edges) {
            ++drops;
            return;
        }
        Add(pending[edge], Stats{1, bytes, recent ? bytes : 0, cpu_dirty ? 1U : 0U});
    }

    size_t max_ranges, max_edges;
    u64 active_shader{}, drops{}, resets{};
    std::map<DAddr, Range> ranges;
    std::map<Edge, Stats> pending, edges;
};

} // namespace VideoCommon
