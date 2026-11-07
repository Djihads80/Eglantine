// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "video_core/macro_turing.h"

// Nx2Perf front_macro_memo: the split front replays a native macro call from a table instead of
// entering the JIT. The JIT resets its State per call (or keeps only fields written before they
// are read), so a call whose code touches no data RAM emits a pure function of its parameters
// and the register values its reads return. A start qualifies when a static scan of its
// reachable code finds no data RAM op, no immediate MME DMA or macro method and no backward
// branch; a call is recorded only when its emits avoid those methods too, so a hit replays
// exactly what the JIT would emit.
namespace VideoCommon::FrontMacroMemo {

inline constexpr size_t MaxParameters = 8;
inline constexpr size_t MaxReads = 4;
inline constexpr size_t MaxEmits = 16;
inline constexpr size_t Entries = 1024;
inline constexpr u32 MaxInstructions = 0x8000 / 3 + 1; ///< MacroEngine::native_code
inline constexpr u32 MaxScan = 512;                    ///< Reachable instructions per start
inline constexpr u32 MaxMacros = 256;                  ///< Per-macro counters (+1 for the rest)

struct Pair {
    u32 first;
    u32 second;
    constexpr bool operator==(const Pair&) const = default;
};

struct Entry {
    bool valid{};
    u64 generation{};
    u32 start{};
    u32 num_parameters{};
    u32 num_reads{};
    u32 num_emits{};
    std::array<u32, MaxParameters> parameters{};
    std::array<Pair, MaxReads> reads{};  ///< (register, value) the call's reads returned
    std::array<Pair, MaxEmits> emits{};  ///< (method, value) in order
};

[[nodiscard]] constexpr bool ExcludedMethod(u32 method, u32 macro_registers_start) {
    return method >= macro_registers_start || (method >= 0x154 && method <= 0x158);
}

/// Empty when `start` qualifies, else why not.
inline std::string_view Classify(std::span<const u32> code, u32 start, u32 macro_registers_start) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    const u32 count = static_cast<u32>((std::min<size_t>)(code.size() / 3, MaxInstructions));
    if (start >= count) {
        return "start outside the code";
    }
    // Bit 0: reached normally; bit 1: reached as the instruction after an END (runs, then the
    // macro ends).
    std::array<u8, MaxInstructions> seen{};
    std::array<std::pair<u32, bool>, MaxScan * 2> work{};
    size_t pending = 0;
    u32 scanned = 0;
    std::string_view reason;
    const auto push = [&](std::int64_t pc, bool last) {
        if (pc < 0 || pc >= count) {
            return; // The JIT fails there; failed calls are not recorded.
        }
        const u8 bit = last ? 2 : 1;
        u8& state = seen[static_cast<size_t>(pc)];
        if ((state & 1) || (state & bit)) {
            return;
        }
        state |= bit;
        if (pending == work.size()) {
            reason = "reachable code too large";
            return;
        }
        work[pending++] = {static_cast<u32>(pc), last};
    };
    push(start, false);
    while (pending != 0 && reason.empty()) {
        const auto [pc, last] = work[--pending];
        if (++scanned > MaxScan) {
            return "reachable code too large";
        }
        const auto d = Interpreter::Decode({code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
        for (u32 lane = 0; lane < 2; ++lane) {
            if (d.op[lane] == 30 || d.op[lane] == 31) {
                return "data RAM access";
            }
            u32 value = 0;
            switch (d.out_method[lane]) {
            case 5: value = d.imm[0]; break;
            case 6: value = d.imm[1]; break;
            case 10: value = (d.imm[0] << 16) | d.imm[1]; break;
            default: continue;
            }
            if (ExcludedMethod(value & 0xfff, macro_registers_start)) {
                return "emits an MME DMA or macro method";
            }
        }
        if (last) {
            continue;
        }
        const bool end = d.end;
        push(std::int64_t{pc} + 1, end);
        for (u32 lane = 0; lane < 2; ++lane) {
            const u32 imm = d.imm[lane];
            switch (d.op[lane]) {
            case 23: // LOOP: the body (pc + 1 on), bounded by its count, or the skip
                push(std::int64_t{pc} + imm, end);
                break;
            case 24:
                if ((imm & 0x7fff) == 0) {
                    return "jump to itself";
                }
                push(std::int64_t{pc} + (imm & 0x7fff), end);
                break;
            case 25: case 26: case 27: case 28: case 29: {
                const auto offset = std::int32_t(imm << 19) >> 19;
                if (offset == -4096) {
                    break; // Stops the macro
                }
                if (offset <= 0) {
                    return "backward branch";
                }
                // A taken branch with bit 14 cancels the END.
                push(std::int64_t{pc} + offset, end && (imm & 0x4000) == 0);
                break;
            }
            default:
                break;
            }
        }
    }
    return reason;
}

/// One call's reads and emits as the JIT made them.
struct Recorder {
    std::array<Pair, MaxReads> reads{};
    std::array<Pair, MaxEmits> emits{};
    u32 num_reads{};
    u32 num_emits{};  ///< All emits, also past MaxEmits
    bool overflow{};  ///< Over a limit, or a read of a register an earlier emit may have changed
    bool excluded{};  ///< Emitted an MME DMA or macro method

    /// `touches(method, address)`: whether writing `method` may change register `address`.
    template <typename Touches>
    void Read(u32 address, u32 value, u32 num_regs, Touches&& touches) {
        if (address >= num_regs) {
            return; // Always 0
        }
        for (u32 i = 0; i < (std::min<u32>)(num_emits, MaxEmits); ++i) {
            if (touches(emits[i].first, address)) {
                overflow = true;
                return;
            }
        }
        if (num_reads == MaxReads) {
            overflow = true;
            return;
        }
        reads[num_reads++] = {address, value};
    }

    void Emit(u32 method, u32 value, u32 macro_registers_start) {
        excluded |= ExcludedMethod(method, macro_registers_start);
        if (num_emits < MaxEmits) {
            emits[num_emits] = {method, value};
        } else {
            overflow = true;
        }
        ++num_emits;
    }

    [[nodiscard]] bool Matches(const Entry& entry) const {
        return num_emits == entry.num_emits && num_emits <= MaxEmits &&
               std::equal(emits.begin(), emits.begin() + num_emits, entry.emits.begin());
    }
};

struct Memo {
    std::unique_ptr<std::array<Entry, Entries>> table; ///< Allocated once, on first use
    u64 class_generation{~u64{0}};
    /// Per start: 0 unknown, 1 memoizable, 2 not.
    std::array<u8, MaxInstructions> classes{};
    std::array<u16, MaxInstructions> failed_records{};
    std::array<u8, MaxInstructions> recorded{};

    std::array<u64, MaxMacros + 1> hits{};
    std::array<u64, MaxMacros + 1> misses{};
    std::array<u64, MaxMacros + 1> skipped{};
    u64 verify_checked{};
    u64 verify_differed{};
    u32 calls_since_check{};
    u64 last_log_ns{};

    [[nodiscard]] static constexpr u32 Bucket(u32 macro) {
        return macro < MaxMacros ? macro : MaxMacros;
    }

    bool Memoizable(std::span<const u32> code, u64 generation, u32 start,
                    u32 macro_registers_start) {
        if (!table) [[unlikely]] {
            table = std::make_unique<std::array<Entry, Entries>>();
        }
        if (generation != class_generation) {
            class_generation = generation;
            classes.fill(0);
            failed_records.fill(0);
            recorded.fill(0);
        }
        if (start >= MaxInstructions) {
            return false;
        }
        u8& cls = classes[start];
        if (cls == 0) [[unlikely]] {
            cls = Classify(code, start, macro_registers_start).empty() ? 1 : 2;
        }
        return cls == 1;
    }

    [[nodiscard]] static size_t Index(u32 start, std::span<const u32> parameters) {
        u64 h = (u64{start} << 8 | parameters.size()) * 0x9E3779B97F4A7C15ULL;
        for (const u32 word : parameters) {
            h = (h ^ word) * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 29;
        }
        return static_cast<size_t>(h >> 40) & (Entries - 1);
    }

    /// The entry for this call when every recorded read still matches: `read_ok(address,
    /// value)` says whether the register holds that value now.
    template <typename ReadOk>
    const Entry* Find(u64 generation, u32 start, std::span<const u32> parameters,
                      ReadOk&& read_ok) const {
        if (!table || parameters.size() > MaxParameters) {
            return nullptr;
        }
        const Entry& entry = (*table)[Index(start, parameters)];
        if (!entry.valid || entry.generation != generation || entry.start != start ||
            entry.num_parameters != parameters.size() ||
            !std::equal(parameters.begin(), parameters.end(), entry.parameters.begin())) {
            return nullptr;
        }
        for (u32 i = 0; i < entry.num_reads; ++i) {
            if (!read_ok(entry.reads[i].first, entry.reads[i].second)) {
                return nullptr;
            }
        }
        return &entry;
    }

    /// Stores a successful call's record, or counts why it could not be.
    void Store(u64 generation, u32 start, std::span<const u32> parameters, const Recorder& rec) {
        if (start >= MaxInstructions || !table) {
            return;
        }
        if (rec.excluded) {
            classes[start] = 2;
            return;
        }
        if (rec.overflow || parameters.size() > MaxParameters) {
            // A start that never fits the limits stops being tried.
            if (!recorded[start] && ++failed_records[start] >= 256) {
                classes[start] = 2;
            }
            return;
        }
        Entry& entry = (*table)[Index(start, parameters)];
        entry.valid = true;
        entry.generation = generation;
        entry.start = start;
        entry.num_parameters = static_cast<u32>(parameters.size());
        std::copy(parameters.begin(), parameters.end(), entry.parameters.begin());
        entry.num_reads = rec.num_reads;
        std::copy_n(rec.reads.begin(), rec.num_reads, entry.reads.begin());
        entry.num_emits = rec.num_emits;
        std::copy_n(rec.emits.begin(), rec.num_emits, entry.emits.begin());
        recorded[start] = 1;
    }
};

} // namespace VideoCommon::FrontMacroMemo
