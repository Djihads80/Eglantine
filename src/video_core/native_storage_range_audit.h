// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

#include "common/common_types.h"

namespace VideoCommon::NativeStorageRangeAudit {

// Diagnostic only. A qualifying word after a native pointer is NOT proof of a descriptor.
// The candidate is never applied to a binding, shader access, dirty range, or writeback.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* gate = std::getenv("EDEN_NX2_STORAGE_RANGE_AUDIT");
        const char* path = std::getenv("EDEN_NX2_STORAGE_RANGE_OUT");
        return gate && std::string_view{gate} == "1" && path && *path;
    }();
    return enabled;
}

inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline u64 Seconds(const char* name, double fallback, double maximum) {
    const char* value = std::getenv(name);
    char* end{};
    const double parsed = value ? std::strtod(value, &end) : fallback;
    const double seconds = !std::isfinite(parsed) || parsed < 0.0 || parsed > maximum ||
                                   (value && (end == value || *end != '\0'))
                               ? fallback : parsed;
    return static_cast<u64>(seconds * 1.0e9);
}

struct Key {
    u64 shader_hash{};
    u64 native_key{};
    u32 cbuf{}, offset{};
    bool written{};
    bool operator==(const Key&) const = default;
};

struct Row {
    Key key{};
    bool used{};
    u64 calls{}, dispatches{}, last_dispatch{}, unknown{}, smaller{}, larger{};
    u64 current_bytes{}, candidate_bytes{}, possible_reduction_bytes{};
    u32 min_current{~u32{}}, max_current{}, min_candidate{~u32{}}, max_candidate{};
    u32 flags_or{}, written_descriptors{};
    u64 first_program_address{}, last_program_address{};
    std::array<u32, 6> first_grid_block{}, last_grid_block{};
};

class State {
public:
    State() : owner{std::this_thread::get_id()}, path{std::getenv("EDEN_NX2_STORAGE_RANGE_OUT")} {
        begin_ns = Now() + Seconds("EDEN_NX2_STORAGE_RANGE_START", 120.0, 3600.0);
        end_ns = begin_ns + Seconds("EDEN_NX2_STORAGE_RANGE_SECONDS", 5.0, 30.0);
    }
    ~State() { Report(false); }

    bool Enter() {
        if (std::this_thread::get_id() != owner || reported) return false;
        const u64 now = Now(); // Once per native Configure; never per method or binding.
        if (now >= end_ns) { Report(true); return false; }
        if (now < begin_ns) return false;
        ++dispatches;
        return true;
    }

    void Add(const Key& key, u32 current, u32 candidate, bool known, bool expanded,
             u32 flags, u32 written_descriptors, const std::array<u32, 6>& grid_block,
             u64 program_address) {
        ++bindings;
        if (!known) ++unknown;
        if (expanded) ++larger;
        current_bytes += current;
        candidate_bytes += candidate;
        const u64 reduction = current > candidate ? current - candidate : 0;
        possible_reduction_bytes += reduction;
        if (key.written) possible_written_reduction_bytes += reduction;
        size_t slot = static_cast<size_t>(key.shader_hash ^ (u64{key.cbuf} << 40) ^
                                         (u64{key.offset} << 1) ^ key.written) % rows.size();
        for (size_t probe = 0; probe < rows.size(); ++probe, slot = (slot + 1) % rows.size()) {
            Row& row = rows[slot];
            if (row.used && !(row.key == key)) continue;
            if (!row.used) {
                row.used = true; row.key = key; row.first_grid_block = grid_block;
                row.first_program_address = program_address;
            }
            row.last_grid_block = grid_block;
            row.last_program_address = program_address;
            row.flags_or |= flags;
            row.written_descriptors = written_descriptors;
            ++row.calls;
            if (row.last_dispatch != dispatches) { ++row.dispatches; row.last_dispatch = dispatches; }
            row.unknown += !known;
            row.smaller += candidate < current;
            row.larger += expanded;
            row.current_bytes += current;
            row.candidate_bytes += candidate;
            row.possible_reduction_bytes += reduction;
            row.min_current = std::min(row.min_current, current);
            row.max_current = std::max(row.max_current, current);
            if (known && !expanded) {
                row.min_candidate = std::min(row.min_candidate, candidate);
                row.max_candidate = std::max(row.max_candidate, candidate);
            }
            return;
        }
        ++capacity_skips;
    }

private:
    void Report(bool window_completed) {
        if (reported) return;
        reported = true;
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (!file) return;
        std::array<const Row*, 8> top{};
        std::array<const Row*, 8> top_written{};
        for (const Row& row : rows) {
            if (!row.used) continue;
            if (row.key.written) {
                for (size_t index = 0; index < top_written.size(); ++index) {
                    if (!top_written[index] || row.current_bytes > top_written[index]->current_bytes) {
                        for (size_t tail = top_written.size() - 1; tail > index; --tail)
                            top_written[tail] = top_written[tail - 1];
                        top_written[index] = &row;
                        break;
                    }
                }
            }
            for (size_t index = 0; index < top.size(); ++index) {
                if (!top[index] || row.possible_reduction_bytes > top[index]->possible_reduction_bytes) {
                    for (size_t tail = top.size() - 1; tail > index; --tail) top[tail] = top[tail - 1];
                    top[index] = &row;
                    break;
                }
            }
        }
        const auto number = [](u64 value) { return static_cast<unsigned long long>(value); };
        std::fprintf(file,
            "{\"key_kind\":\"lowered_compute_shader_hash\",\"window_completed\":%s,\"window_cpu_begin_ns\":%llu,"
            "\"window_cpu_end_ns\":%llu,\"dispatches\":%llu,\"bindings\":%llu,"
            "\"unknown\":%llu,\"candidate_larger_not_measured\":%llu,\"capacity_skips\":%llu,"
            "\"current_binding_bytes_sum\":%llu,\"candidate_binding_bytes_sum\":%llu,"
            "\"possible_reduction_bytes_sum\":%llu,\"possible_written_reduction_bytes_sum\":%llu,"
            "\"notes\":[\"No binding or guest-visible behavior changed.\","
            "\"Byte sums count repeated bindings, not distinct storage or actual downloads.\","
            "\"Candidates are unproven next-qword heuristics; never a safe shrink recommendation.\","
            "\"Unknown and expanding candidates retain current size in sums.\","
            "\"flags_or bits: 1=native_global_bounds,2=uses_global_memory,4=indirect_dispatch.\","
            "\"native_key zero means unknown (for example RESETFIX=0); no new program hash is computed.\","
            "\"Window begins relative to first native Configure; reads are CPU-side, without GPU flush.\"],"
            "\"top8\":[",
            window_completed ? "true" : "false", number(begin_ns), number(end_ns), number(dispatches), number(bindings), number(unknown),
            number(larger), number(capacity_skips), number(current_bytes), number(candidate_bytes),
            number(possible_reduction_bytes), number(possible_written_reduction_bytes));
        bool comma = false;
        for (const Row* row : top) {
            if (!row) continue;
            std::fprintf(file,
                "%s{\"shader_hash\":\"%016llx\",\"native_key\":\"%016llx\",\"cbuf\":%u,\"offset\":%u,\"written\":%s,"
                "\"calls\":%llu,\"dispatches\":%llu,\"unknown\":%llu,\"smaller\":%llu,"
                "\"larger_not_measured\":%llu,\"current_bytes_sum\":%llu,\"candidate_bytes_sum\":%llu,"
                "\"possible_reduction_bytes_sum\":%llu,\"min_current\":%u,\"max_current\":%u,"
                "\"min_candidate\":%u,\"max_candidate\":%u,\"flags_or\":%u,"
                "\"written_descriptors\":%u,\"first_grid_block\":[%u,%u,%u,%u,%u,%u],"
                "\"last_grid_block\":[%u,%u,%u,%u,%u,%u],\"first_program_address\":\"%llx\","
                "\"last_program_address\":\"%llx\"}",
                comma ? "," : "", number(row->key.shader_hash), number(row->key.native_key), row->key.cbuf, row->key.offset,
                row->key.written ? "true" : "false", number(row->calls), number(row->dispatches),
                number(row->unknown), number(row->smaller), number(row->larger), number(row->current_bytes),
                number(row->candidate_bytes), number(row->possible_reduction_bytes), row->min_current,
                row->max_current, row->min_candidate, row->max_candidate, row->flags_or,
                row->written_descriptors, row->first_grid_block[0], row->first_grid_block[1],
                row->first_grid_block[2], row->first_grid_block[3], row->first_grid_block[4],
                row->first_grid_block[5], row->last_grid_block[0], row->last_grid_block[1],
                row->last_grid_block[2], row->last_grid_block[3], row->last_grid_block[4],
                row->last_grid_block[5], number(row->first_program_address), number(row->last_program_address));
            comma = true;
        }
        std::fputs("],\"top8_written_current_bytes\":[", file);
        comma = false;
        for (const Row* row : top_written) {
            if (!row) continue;
            std::fprintf(file,
                "%s{\"shader_hash\":\"%016llx\",\"native_key\":\"%016llx\",\"cbuf\":%u,\"offset\":%u,"
                "\"calls\":%llu,\"dispatches\":%llu,\"current_bytes_sum\":%llu,"
                "\"candidate_bytes_sum\":%llu,\"possible_reduction_bytes_sum\":%llu,"
                "\"min_current\":%u,\"max_current\":%u,\"flags_or\":%u,\"written_descriptors\":%u,"
                "\"first_program_address\":\"%llx\",\"last_program_address\":\"%llx\","
                "\"first_grid_block\":[%u,%u,%u,%u,%u,%u],\"last_grid_block\":[%u,%u,%u,%u,%u,%u]}",
                comma ? "," : "", number(row->key.shader_hash), number(row->key.native_key),
                row->key.cbuf, row->key.offset, number(row->calls), number(row->dispatches),
                number(row->current_bytes), number(row->candidate_bytes), number(row->possible_reduction_bytes),
                row->min_current, row->max_current, row->flags_or, row->written_descriptors,
                number(row->first_program_address), number(row->last_program_address),
                row->first_grid_block[0], row->first_grid_block[1], row->first_grid_block[2],
                row->first_grid_block[3], row->first_grid_block[4], row->first_grid_block[5],
                row->last_grid_block[0], row->last_grid_block[1], row->last_grid_block[2],
                row->last_grid_block[3], row->last_grid_block[4], row->last_grid_block[5]);
            comma = true;
        }
        std::fputs("],\"focused_native_programs\":[", file);
        constexpr std::array<u64, 3> focus{0xe1d1cd3cc602baccULL, 0xc1d394dc24239c40ULL, 0x20ab4117f4db5bdbULL};
        comma = false;
        for (const u64 native_key : focus) {
            u64 calls{}, written_calls{}, bytes{}, reduction{};
            for (const Row& row : rows) {
                if (!row.used || row.key.native_key != native_key) continue;
                calls += row.calls;
                written_calls += row.key.written ? row.calls : 0;
                bytes += row.current_bytes;
                reduction += row.possible_reduction_bytes;
            }
            std::fprintf(file, "%s{\"native_key\":\"%016llx\",\"binding_calls\":%llu,"
                "\"written_binding_calls\":%llu,\"current_bytes_sum\":%llu,\"possible_reduction_bytes_sum\":%llu}",
                comma ? "," : "", number(native_key), number(calls), number(written_calls), number(bytes), number(reduction));
            comma = true;
        }
        std::fputs("]}\n", file);
        std::fclose(file);
    }
    const std::thread::id owner;
    std::string path;
    std::array<Row, 1024> rows{};
    u64 begin_ns{}, end_ns{}, dispatches{}, bindings{}, unknown{}, larger{}, capacity_skips{};
    u64 current_bytes{}, candidate_bytes{}, possible_reduction_bytes{}, possible_written_reduction_bytes{};
    bool reported{};
};

class Scope {
public:
    Scope(u64 shader_hash, bool native, u64 program_address_, u64 native_key)
        : previous{current}, program_address{program_address_} {
        if (native && Enabled()) {
            static State state;
            if (state.Enter()) { sink = &state; key.shader_hash = shader_hash; key.native_key = native_key; }
        }
        current = sink ? this : nullptr;
    }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    explicit operator bool() const { return sink != nullptr; }
    void Dispatch(u32 flags_, u32 written_descriptors_, const std::array<u32, 6>& grid_block_) {
        flags = flags_; written_descriptors = written_descriptors_; grid_block = grid_block_;
    }
    void Binding(u32 cbuf, u32 offset, bool written, u32 cbuf_size) {
        key.cbuf = cbuf; key.offset = offset; key.written = written; cbuf_bytes = cbuf_size;
    }

    template <typename Memory>
    static void Observe(Memory& memory, u64 pointer_location, u32 layout_size, u32 raw_size,
                        u32 binding_size, u32 alignment_prefix, u32 page_size, u32 base_page_offset) {
        Scope* scope = current;
        if (!scope) return;
        u32 candidate = binding_size;
        bool known = false, expanded = false;
        // Read only the additional eight bytes the alternate heuristic would consume; first
        // verify the active QMD constant-buffer and GPU mappings. No GPU readback or cache flush.
        if (scope->key.offset <= scope->cbuf_bytes && scope->cbuf_bytes - scope->key.offset >= 16 &&
            pointer_location <= std::numeric_limits<u64>::max() - 16 &&
            memory.IsFullyMappedRange(pointer_location + 8, 8)) {
            const u64 next = memory.template Read<u64>(pointer_location + 8);
            const u32 packed = static_cast<u32>(next);
            known = true;
            if ((next >> 32) == 0 && packed != 0 && packed <= layout_size) {
                if (packed <= raw_size) {
                    const u64 aligned = u64{alignment_prefix} + packed;
                    const u64 rounded = scope->key.written ? aligned :
                        ((base_page_offset + aligned + page_size - 1) / page_size) * page_size - base_page_offset;
                    if (rounded <= binding_size) candidate = static_cast<u32>(rounded);
                } else {
                    // A larger range would need another page walk; this audit only measures
                    // potential shrink, so keep current size and count expansion separately.
                    expanded = true;
                }
            }
        }
        scope->sink->Add(scope->key, binding_size, candidate, known, expanded, scope->flags,
                         scope->written_descriptors, scope->grid_block, scope->program_address);
    }
private:
    inline static thread_local Scope* current{};
    Scope* previous{};
    State* sink{};
    Key key{};
    u32 cbuf_bytes{};
    u32 flags{}, written_descriptors{};
    u64 program_address{};
    std::array<u32, 6> grid_block{};
};

} // namespace VideoCommon::NativeStorageRangeAudit
