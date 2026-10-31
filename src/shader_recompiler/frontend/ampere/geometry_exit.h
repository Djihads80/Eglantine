// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Shader::Ampere {

struct GeometryExitEnvelope {
    struct Exit {
        u32 offset;
        u32 predicate;
    };
    std::vector<Exit> exits;
    u32 handler{};
    u32 error_offset{};
    std::string_view error;

    explicit operator bool() const { return error.empty(); }
};

inline constexpr u32 GeometryExitSetupInstructions = 5;

template <typename Code>
GeometryExitEnvelope AnalyzeGeometryExit(const Code& code) {
    constexpr u32 ModeMask = 0xc00001ffU;
    constexpr size_t Setup = GeometryExitSetupInstructions;
    GeometryExitEnvelope result;
    const auto fail = [&](size_t index, std::string_view reason) {
        result.exits.clear();
        result.handler = 0;
        result.error_offset = static_cast<u32>(index * 16);
        result.error = reason;
        return result;
    };
    const auto bits = [&](size_t index, u32 offset, u32 count) {
        const auto& words = code[index].words;
        u64 value = words[offset / 32];
        if (offset / 32 + 1 < 4) value |= u64{words[offset / 32 + 1]} << 32;
        return static_cast<u32>((value >> (offset % 32)) & ((u64{1} << count) - 1));
    };
    const auto is = [&](size_t index, u32 word0, u32 word1, u32 word2) {
        const auto& words = code[index].words;
        return words[0] == word0 && words[1] == word1 && words[2] == word2 &&
               (words[3] & ModeMask) == 0;
    };
    const auto is_exit = [&](size_t index) {
        const auto& words = code[index].words;
        return (words[0] & ~0xf000U) == 0x94d && words[1] == 0 && words[2] == 0x03800000 &&
               (words[3] & ModeMask) == 0 && bits(index, 12, 4) != 15;
    };

    const size_t count = code.size();
    if (count < Setup + 4) return fail(count, "native exit envelope truncated");
    const u32 target = code[1].words[1];
    if (!is(0, 0x734e, 0, 0) || !is(1, 0x37802, target, 0xf00) || !is(2, 0x7210, 3, 0x07f1e0ff) ||
        !(is(3, 0x01017210, 0xff, 0x007fe4ff) || is(3, 0xff017224, 0xff, 0x000e0401)) ||
        !is(4, 0x7357, 0, 0))
        return fail(0, "native exit handler installation");
    const size_t handler = target / 16;
    if (target % 16 || handler <= Setup + 1 || handler + 3 != count)
        return fail(1, "native exit handler location");
    if (!is(handler, 0x00ff7324, 0xff, 0) || !is(handler + 1, 0x7957, 0, 0) ||
        !is(handler + 2, 0x794d, 0, 0x03800000))
        return fail(handler, "native exit handler body");
    if (!is_exit(handler - 1) || bits(handler - 1, 12, 4) != 7)
        return fail(handler - 1, "native exit handler reached by fallthrough");

    for (size_t index = Setup; index < handler; ++index) {
        const u32 opcode = bits(index, 0, 12), family = opcode & 0x1ff;
        if (family == 0x14e || (family >= 0x154 && family <= 0x157))
            return fail(index, "native exit handler state access in body");
        if (opcode == 0x94d) {
            if (!is_exit(index)) return fail(index, "native body EXIT mode");
            result.exits.push_back({static_cast<u32>(index * 16), bits(index, 12, 4)});
            continue;
        }
        if (family < 0x140 || family >= 0x160 || opcode == 0x941 || opcode == 0x942 ||
            opcode == 0x948)
            continue;
        const auto& words = code[index].words;
        const bool noinc_call = opcode == 0x944 && (words[2] & 0x400000) != 0;
        if (opcode != 0x945 && opcode != 0x947 && !noinc_call)
            return fail(index, "native control flow in exit envelope body");
        const u64 raw = words[1] | (u64{words[2] & 0x3ffff} << 32);
        const s64 relative = opcode == 0x945 ? s64{static_cast<s32>(words[1])}
                                             : static_cast<s64>(raw << 14) >> 14;
        const s64 destination = static_cast<s64>(index) * 16 + 16 + relative;
        if (destination % 16 || destination < static_cast<s64>(Setup * 16) ||
            destination >= static_cast<s64>(handler * 16))
            return fail(index, "native branch leaves exit envelope body");
    }

    u32 live_registers = 0b1011, live_predicates = 0b1;
    for (size_t index = Setup; live_registers || live_predicates; ++index) {
        const u32 opcode = bits(index, 0, 12), family = opcode & 0x1ff;
        const u32 form = (opcode >> 9) & 7, guard = bits(index, 12, 4);
        u32 reads = 0, read_predicates = 0, writes = 0, write_predicates = 0;
        const auto read_register = [&](u32 reg) { if (reg < 32) reads |= 1U << reg; };
        const auto write_register = [&](u32 reg) { if (reg < 32) writes |= 1U << reg; };
        const auto read_predicate = [&](u32 pred) { if (pred < 7) read_predicates |= 1U << pred; };
        const auto write_predicate = [&](u32 pred) { if (pred < 7) write_predicates |= 1U << pred; };
        const auto read_sources = [&](bool has_c) {
            read_register(bits(index, 24, 8));
            if (form == 1) read_register(bits(index, 32, 8));
            if (form == 2 || form == 3 || form == 7) read_register(bits(index, 64, 8));
            if (has_c && (form == 1 || form == 4 || form == 5 || form == 6)) read_register(bits(index, 64, 8));
        };
        read_predicate(guard & 7);
        if (opcode == 0x919 || opcode == 0x923) {
            if (opcode == 0x923) read_register(bits(index, 24, 8));
            write_register(bits(index, 16, 8));
        } else if (opcode == 0x805 || opcode == 0xab9 || opcode == 0x945) {
        } else if (opcode == 0x381 || opcode == 0x981) {
            const u32 address = bits(index, 24, 8), size = bits(index, 73, 3);
            if (address != 255) {
                read_register(address);
                read_register(address + 1);
            }
            const u32 results = size < 5 ? 1 : size == 5 ? 2 : size == 6 ? 4 : 0;
            for (u32 i = 0; i < results; ++i) write_register(bits(index, 16, 8) + i);
        } else if (family == 0x002 && (form == 1 || form == 4 || form == 5 || form == 6)) {
            if (form == 1) read_register(bits(index, 32, 8));
            write_register(bits(index, 16, 8));
        } else if ((family == 0x024 || family == 0x016) && form != 0) {
            read_sources(true);
            if (family == 0x024 && bits(index, 74, 1)) read_predicate(bits(index, 87, 3));
            write_register(bits(index, 16, 8));
        } else if (family == 0x00c && form != 0 && !bits(index, 72, 1)) {
            read_sources(false);
            read_predicate(bits(index, 87, 3));
            write_predicate(bits(index, 81, 3));
            write_predicate(bits(index, 84, 3));
        } else {
            return fail(index, "native instruction before exit setup state is dead");
        }
        if ((reads & live_registers) || (read_predicates & live_predicates))
            return fail(index, "native body reads exit setup state");
        if (guard == 7) {
            live_registers &= ~writes;
            live_predicates &= ~write_predicates;
        }
    }
    result.handler = target;
    return result;
}

} // namespace Shader::Ampere
