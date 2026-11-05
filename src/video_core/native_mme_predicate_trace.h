// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <type_traits>
#include <fmt/ranges.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/query_cache/native_query_producer_trace.h"

namespace VideoCommon::NativeMmePredicateTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MME_PREDICATE_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Macro {
    u64 sequence{};
    u32 method{};
    u32 start{};
    u32 parameter_count{};
    u32 sampled{};
    std::array<u32, 8> parameters{};
};

struct DmaRead {
    u64 sequence{};
    u64 macro_sequence{};
    u64 source{};
    u64 asid{};
    u32 macro_start{};
    u32 method{};
    u32 ram{};
    u32 requested{};
    u32 read_count{};
    u32 sampled{};
    std::array<u32, 8> words{};
};

// No pointers into parameters, data RAM, or the front's mutable ring. GpuSplit::Defer copies
// this value, which remains valid after the front starts another macro.
struct Snapshot {
    Macro macro{};
    u64 predicate_sequence{};
    u32 raw_value{};
    u32 effective_value{};
    u32 shadow_control{};
    bool from_macro{};
    u32 dma_count{};
    u32 dma_next{};
    std::array<DmaRead, 8> dma{};
};
static_assert(std::is_trivially_copyable_v<Snapshot>);
static_assert(alignof(Snapshot) <= alignof(u64));

// Only Maxwell3D's front accesses this state. Logical sequence numbers avoid clock reads in
// the DMA loop; the eventual bounded log supplies a host timestamp.
struct FrontTrace {
    Macro macro{};
    u64 macro_sequence{};
    u64 dma_sequence{};
    u64 predicate_sequence{};
    u32 dma_count{};
    u32 dma_next{};
    std::array<DmaRead, 8> dma{};

    void BeginMacro(u32 method, u32 start, std::span<const u32> parameters) {
        macro = {};
        macro.sequence = ++macro_sequence;
        macro.method = method;
        macro.start = start;
        macro.parameter_count = static_cast<u32>(parameters.size());
        macro.sampled = static_cast<u32>(std::min(parameters.size(), macro.parameters.size()));
        std::copy_n(parameters.begin(), macro.sampled, macro.parameters.begin());
    }

    DmaRead& BeginDma(u64 source, u32 method, u32 ram, u32 requested, u32 read_count, u64 asid) {
        auto& record = dma[dma_next];
        record = {};
        record.sequence = ++dma_sequence;
        record.macro_sequence = macro.sequence;
        record.macro_start = macro.start;
        record.source = source;
        record.asid = asid;
        record.method = method;
        record.ram = ram;
        record.requested = requested;
        record.read_count = read_count;
        record.sampled = std::min<u32>(read_count, record.words.size());
        dma_next = (dma_next + 1) % dma.size();
        dma_count = std::min<u32>(dma_count + 1, dma.size());
        return record;
    }

    Snapshot Predicate(u32 raw, u32 effective, u32 shadow, bool from_macro) {
        return {.macro = from_macro ? macro : Macro{},
                .predicate_sequence = ++predicate_sequence,
                .raw_value = raw,
                .effective_value = effective,
                .shadow_control = shadow,
                .from_macro = from_macro,
                .dma_count = dma_count,
                .dma_next = dma_next,
                .dma = dma};
    }
};

struct BackRecord {
    const void* engine{};
    bool valid{};
    Snapshot snapshot{};
};

// Only the back installs and reads records. An evicted engine reports unavailable rather
// than borrowing another engine's predicate. Memory use stays fixed across channel changes.
inline BackRecord& Record(const void* engine) {
    static thread_local std::array<BackRecord, 8> records{};
    static thread_local size_t next{};
    for (auto& record : records) {
        if (record.engine == engine) {
            return record;
        }
    }
    auto& record = records[next];
    next = (next + 1) % records.size();
    record = {};
    record.engine = engine;
    return record;
}

inline void Reset(const void* engine) {
    Record(engine).valid = false;
}

inline void Install(const void* engine, const Snapshot& snapshot) {
    auto& record = Record(engine);
    record.snapshot = snapshot;
    record.valid = true;
}

// Call only within the draw3216 detail budget. No guest-memory or register reads occur here.
inline void Log(const void* engine, bool execute, u32 current_mode) {
    const auto& record = Record(engine);
    if (!record.valid) {
        LOG_INFO(HW_GPU, "NX2 predicate origin: unavailable execute={} mode={}", execute, current_mode);
        return;
    }
    const auto& snapshot = record.snapshot;
    const auto& macro = snapshot.macro;
    LOG_INFO(HW_GPU,
             "NX2 predicate origin: predicate={} execute={} mode={} front_effective={} raw={} "
             "shadow={} from_macro={} macro={} method={:#x} start={:#x} params={}/{} [{:08x}] dma={}",
             snapshot.predicate_sequence, execute, current_mode, snapshot.effective_value,
             snapshot.raw_value, snapshot.shadow_control, snapshot.from_macro, macro.sequence,
             macro.method, macro.start, macro.sampled, macro.parameter_count,
             fmt::join(std::span{macro.parameters}.first(macro.sampled), " "), snapshot.dma_count);
    for (u32 i = 0; i < snapshot.dma_count; ++i) {
        const auto& dma = snapshot.dma[(snapshot.dma_next + snapshot.dma.size() -
                                       snapshot.dma_count + i) % snapshot.dma.size()];
        LOG_INFO(HW_GPU,
                 "NX2 predicate DMA: predicate={} read={} macro={} start={:#x} source={:#x} "
                 "asid={} method={:#x} ram={:#x} requested={} read_count={} sampled={} words={:08x}",
                 snapshot.predicate_sequence, dma.sequence, dma.macro_sequence, dma.macro_start,
                 dma.source, dma.asid, dma.method, dma.ram, dma.requested, dma.read_count, dma.sampled,
                 fmt::join(std::span{dma.words}.first(dma.sampled), " "));
        if (dma.macro_sequence == macro.sequence && dma.sampled >= 4) {
            NativeQueryProducerTrace::LogConsumed(
                dma.asid, dma.source, u64{dma.words[0]} | (u64{dma.words[1]} << 32),
                u64{dma.words[2]} | (u64{dma.words[3]} << 32));
        }
    }
}

} // namespace VideoCommon::NativeMmePredicateTrace
