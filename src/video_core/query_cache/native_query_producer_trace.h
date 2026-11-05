// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::NativeQueryProducerTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MME_PREDICATE_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Counters {
    u64 begins{}, ends{}, resets{}, begins_at_reset{}, ends_at_reset{};
};
struct Draw {
    u64 asid{}, vs{}, ps{}, rt{}, depth{};
    u32 kind{}, vertices{}, instances{}, depth_enable{}, depth_write{}, depth_func{};
    u32 cull_enable{}, cull_face{}, front_face{};
    float scale_x{}, scale_y{}, scale_z{}, offset_x{}, offset_y{}, offset_z{};
};
struct DrawRange {
    u64 count{};
    Draw first{}, last{};
    void Add(const Draw& draw) {
        if (count == 0) first = draw;
        last = draw;
        ++count;
    }
    void Append(const DrawRange& other) {
        if (!other.count) return;
        if (!count) first = other.first;
        last = other.last;
        count += other.count;
    }
};
struct Samples {
    bool present{}, no_current{}, active_before_report{};
    u64 bank{}, banks{}, slot{}, slots{}, tick{};
    Counters counters{};
    DrawRange current_draws{}, prior_draws{};
};
struct PendingSamples {
    const void* query{};
    Samples samples{};
};
inline PendingSamples& Pending() {
    static thread_local PendingSamples pending;
    return pending;
}
inline void StageSamples(const void* query, const Samples& samples) {
    if (Enabled()) [[unlikely]] Pending() = {query, samples};
}

struct Token {
    u64 sequence{};
    size_t slot{};
};
enum class Outcome : u32 { Pending, Written, Invalidated, Unsynchronized, PartialWrite };
struct Record {
    const void* query{}; // Identity only; never dereferenced by diagnostics.
    u64 sequence{}, asid{}, gpu{}, cpu{}, resolved_value{}, written_value{}, timestamp{};
    u32 type{}, flags{}, payload{};
    bool resolved{}, timestamp_written{};
    Outcome outcome{};
    Samples samples{};
};
struct DirectoryEntry {
    const void* query{};
    Token token{};
};
struct State {
    std::mutex mutex;
    // Keep historical reports after a query pool reuses its objects. The four-way directory
    // tracks only each live object's latest token; old callbacks use their captured token.
    std::array<Record, 32768> records{};
    std::array<DirectoryEntry, 32768> directory{};
    u64 sequence{};
};
inline State& Get() {
    static State state;
    return state;
}
inline size_t Bucket(const void* query) {
    return ((reinterpret_cast<std::uintptr_t>(query) >> 4) * 11400714819323198485ULL % 8192) * 4;
}
inline size_t Slot(State& state, const void* query) {
    const size_t first = Bucket(query);
    size_t oldest = first;
    for (size_t i = first; i < first + 4; ++i) {
        if (state.directory[i].query == query) return i;
        if (state.directory[i].token.sequence < state.directory[oldest].token.sequence) oldest = i;
    }
    return oldest;
}
inline Token Report(const void* query, u64 asid, u64 gpu, u64 cpu, u32 type, u32 flags,
                    u32 payload) {
    if (!Enabled()) return {};
    auto& state = Get();
    const std::scoped_lock lock{state.mutex};
    const u64 sequence = ++state.sequence;
    const size_t slot = (sequence - 1) % state.records.size();
    const Token token{sequence, slot};
    state.directory[Slot(state, query)] = {query, token};
    auto& record = state.records[slot];
    record = {};
    record.query = query;
    record.sequence = sequence;
    record.asid = asid;
    record.gpu = gpu;
    record.cpu = cpu;
    record.type = type;
    record.flags = flags;
    record.payload = payload;
    if (Pending().query == query) record.samples = Pending().samples;
    Pending() = {};
    return token;
}
inline void Resolved(const void* query, u64 cpu, u64 value) {
    if (!Enabled()) return;
    auto& state = Get();
    const std::scoped_lock lock{state.mutex};
    const auto& entry = state.directory[Slot(state, query)];
    if (entry.query != query || !entry.token.sequence) return;
    auto& record = state.records[entry.token.slot];
    if (record.sequence != entry.token.sequence || record.cpu != cpu) return;
    record.resolved = true;
    record.resolved_value = value;
}
inline void Complete(Token token, Outcome outcome, u64 value = 0, u64 timestamp = 0,
                     bool timestamp_written = false) {
    if (!token.sequence) return;
    auto& state = Get();
    const std::scoped_lock lock{state.mutex};
    auto& record = state.records[token.slot];
    if (record.sequence != token.sequence) return;
    record.outcome = outcome;
    record.written_value = value;
    record.timestamp = timestamp;
    record.timestamp_written = timestamp_written;
}
inline void Mark(const void* query, Outcome outcome, u64 value = 0) {
    if (!Enabled()) return;
    auto& state = Get();
    const std::scoped_lock lock{state.mutex};
    const auto& entry = state.directory[Slot(state, query)];
    if (entry.query != query || !entry.token.sequence) return;
    auto& record = state.records[entry.token.slot];
    if (record.sequence != entry.token.sequence) return;
    record.outcome = outcome;
    if (outcome == Outcome::PartialWrite) record.written_value = value;
}

// Called only by the already bounded draw3216 detail logs. A producer may progress between
// DMA consumption and this draw: label an exact value+timestamp match separately from latest.
inline void LogConsumed(u64 asid, u64 gpu, u64 consumed, u64 consumed_timestamp) {
    if (!Enabled()) return;
    Record selected{};
    bool exact = false;
    {
        auto& state = Get();
        const std::scoped_lock lock{state.mutex};
        for (const auto& record : state.records) {
            if (!record.sequence || record.asid != asid || record.gpu != gpu) continue;
            const bool match = record.outcome == Outcome::Written && record.timestamp_written &&
                               record.written_value == consumed && record.timestamp == consumed_timestamp;
            if ((match && !exact) || (match == exact && record.sequence > selected.sequence)) {
                selected = record;
                exact = match;
            }
        }
    }
    if (!selected.sequence) {
        LOG_INFO(HW_GPU, "NX2 query producer: asid={} gpu={:#x} unavailable consumed={:#x} timestamp={:#x}",
                 asid, gpu, consumed, consumed_timestamp);
        return;
    }
    const auto& s = selected.samples;
    LOG_INFO(HW_GPU,
             "NX2 query producer: gpu={:#x} cpu={:#x} asid={} report={} type={} flags={:#x} "
             "payload={:#x} outcome={} exact_consumed={} consumed={:#x}/{:#x} written={:#x}/{:#x} "
             "timestamp_written={} host_resolved={} host_value={:#x} samples={} no_current={} "
             "active_before={} bank={} banks={} slot={} slots={} tick={} begins={} ends={} "
             "resets={} begins_at_reset={} ends_at_reset={}",
             gpu, selected.cpu, selected.asid, selected.sequence, selected.type, selected.flags,
             selected.payload, static_cast<u32>(selected.outcome), exact, consumed, consumed_timestamp,
             selected.written_value, selected.timestamp, selected.timestamp_written, selected.resolved,
             selected.resolved_value, s.present, s.no_current, s.active_before_report, s.bank, s.banks,
             s.slot, s.slots, s.tick, s.counters.begins, s.counters.ends, s.counters.resets,
             s.counters.begins_at_reset, s.counters.ends_at_reset);
    const auto log_draws = [&](const char* segment, const DrawRange& range) {
        if (!range.count) return;
        const auto log_draw = [&](const char* which, const Draw& draw) {
            LOG_INFO(HW_GPU,
                     "NX2 query draw: report={} segment={} recorded_draws={} which={} asid={} "
                     "kind={} vs={:#x} ps={:#x} vertices={} instances={} rt={:#x} depth={:#x} "
                     "depth_enable={} depth_write={} depth_func={} cull_enable={} cull_face={} "
                     "front_face={} viewport_scale={},{},{} viewport_offset={},{},{}",
                     selected.sequence, segment, range.count, which, draw.asid, draw.kind,
                     draw.vs, draw.ps, draw.vertices, draw.instances, draw.rt, draw.depth,
                     draw.depth_enable, draw.depth_write, draw.depth_func, draw.cull_enable,
                     draw.cull_face, draw.front_face, draw.scale_x, draw.scale_y, draw.scale_z,
                     draw.offset_x, draw.offset_y, draw.offset_z);
        };
        log_draw("first", range.first);
        if (range.count > 1) log_draw("last", range.last);
    };
    log_draws("current", s.current_draws);
    log_draws("prior", s.prior_draws);
}
} // namespace VideoCommon::NativeQueryProducerTrace
