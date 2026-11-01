// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Instruction semantics adapted from Mesa mme_tu104_sim.c and mme_tu104.c:
// Copyright © 2022 Collabora Ltd. SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Tegra {

// TU104/GA10x MME has two ALUs and two outputs per 96-bit instruction.
class TuringMacroInterpreter {
public:
    using Word = std::uint32_t;
    struct Result {
        bool success;
        Word pc;
        std::size_t consumed;
        std::string_view error;
    };

    // One instruction's fields, decoded once and reused while its words stay the same (macro
    // code runs thousands of times a frame; decoding it every time was most of the interpreter).
    struct Decoded {
        std::array<Word, 3> raw{~0U, ~0U, ~0U};
        std::array<Word, 2> imm, op, dst, src_a, src_b, out_method, out_emit;
        Word pred_reg, pred_mode;
        // Lane/output enable masks when the predicate register is non-zero / zero.
        Word mask_true, mask_false;
        bool end;
        std::array<bool, 2> load;
    };

    static Decoded Decode(const std::array<Word, 3>& bits) {
        const auto field = [&](Word low, Word count) {
            std::uint64_t value = bits[low / 32];
            if (low / 32 < 2) value |= std::uint64_t{bits[low / 32 + 1]} << 32;
            return Word((value >> (low % 32)) & ((std::uint64_t{1} << count) - 1));
        };
        Decoded d;
        d.raw = bits;
        d.imm = {field(30, 16), field(66, 16)};
        d.op = {field(10, 5), field(46, 5)};
        d.dst = {field(15, 5), field(51, 5)};
        d.src_a = {field(20, 5), field(56, 5)};
        d.src_b = {field(25, 5), field(61, 5)};
        d.out_method = {field(82, 3), field(89, 3)};
        d.out_emit = {field(85, 4), field(92, 4)};
        d.pred_reg = field(5, 5);
        d.pred_mode = field(1, 4);
        d.end = field(0, 1) != 0;
        constexpr std::array<std::string_view, 16> predicates{
            "UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU",
            "UUTT", "UUTF", "UUTU", "UUFT", "UUFF", "UUFU", "UUUT", "UUUF"};
        d.mask_true = d.mask_false = 15;
        if (d.pred_mode) {
            d.mask_true = d.mask_false = 0;
            for (Word i = 0; i < 4; ++i) {
                if (predicates[d.pred_mode][i] != 'T') d.mask_true |= 1U << i;
                if (predicates[d.pred_mode][i] != 'F') d.mask_false |= 1U << i;
            }
        }
        for (Word i = 0; i < 2; ++i) {
            const Word reg = 28 + i, out = 3 + i;
            d.load[i] = d.pred_reg == reg || d.src_a[0] == reg || d.src_a[1] == reg ||
                        d.src_b[0] == reg || d.src_b[1] == reg || d.out_method[0] == out ||
                        d.out_method[1] == out || d.out_emit[0] == out || d.out_emit[1] == out;
        }
        return d;
    }

    struct ExecutionState {
        std::array<Word, 24> regs{};
        Word initialized{}, carry{}, method{}, increment{};
        Word pc{}, loop_count{}, loop_start{}, loop_end{};
        std::size_t consumed{};
        bool end_next{}, stopped{};
    };

    template <typename Read, typename Emit>
    // `parameters` is the load FIFO; `emit` may append to it (MME_DMA_READ_FIFOED).
    Result Execute(std::span<const Word> code, Word start,
                   std::vector<Word>& parameters, Read read, Emit emit) {
        ExecutionState state;
        state.pc = start;
        return Resume(code, state, parameters, read, emit);
    }

    // Resume only at an instruction boundary, so already emitted methods are not replayed.
    template <typename Read, typename Emit>
    Result Resume(std::span<const Word> code, ExecutionState& state,
                  std::vector<Word>& parameters, Read read, Emit emit) {
        auto& [regs, initialized, carry, method, increment, pc, loop_count, loop_start, loop_end,
               consumed, end_next, stopped] = state;
        std::string_view error;
        const auto fail = [&](std::string_view message) { if (error.empty()) error = message; };
        if (decoded.size() < code.size() / 3) decoded.resize(code.size() / 3);
        read_registers = false;
        reread_data_ram = false;
        data_ram_reads.clear();
        data_ram_writes.clear();
        for (Word steps = 0; steps < 1000000; ++steps) {
            if (static_cast<std::size_t>(pc) * 3 + 2 >= code.size()) {
                fail("instruction outside uploaded RAM");
                break;
            }
            const std::array<Word, 3> bits{code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]};
            Decoded& d = decoded[pc];
            if (d.raw != bits) d = Decode(bits);
            std::array<Word, 2> load{}, alu{};
            for (Word i = 0; i < 2; ++i) {
                if (d.load[i]) {
                    if (consumed >= parameters.size()) { fail("parameter FIFO underflow"); break; }
                    load[i] = parameters[consumed++];
                }
            }
            // Both ALUs read the register file before either destination is committed: their
            // operands are read up front. Carry chaining remains explicit between the lanes.
            const auto get = [&](Word reg, Word lane) -> Word {
                if (reg < 24) {
                    if (!(initialized & (1U << reg))) fail("uninitialized register read");
                    return regs[reg];
                }
                switch (reg) {
                case 24: return 0;
                case 25: return Word(std::int32_t(std::int16_t(d.imm[lane])));
                case 26: return Word(std::int32_t(std::int16_t(d.imm[1 - lane])));
                case 27: return (d.imm[0] << 16) | d.imm[1];
                case 28: return load[0];
                case 29: return load[1];
                default: fail("invalid source register"); return 0;
                }
            };
            const Word mask = d.pred_mode ? (get(d.pred_reg, 0) ? d.mask_true : d.mask_false) : 15;
            std::array<Word, 2> xs{}, ys{};
            for (Word lane = 0; lane < 2; ++lane) {
                if (mask & (1U << lane)) {
                    xs[lane] = get(d.src_a[lane], lane);
                    ys[lane] = get(d.src_b[lane], lane);
                }
            }
            Word next_pc = pc + 1;
            bool branch_cancels_end{};
            for (Word lane = 0; lane < 2 && error.empty(); ++lane) {
                if (!(mask & (1U << lane))) continue;
                const Word x = xs[lane], y = ys[lane];
                const Word imm = d.imm[lane];
                Word value{};
                const auto condition = [&](Word comparison) {
                    switch (comparison) {
                    case 17: case 25: return std::int32_t(x) < std::int32_t(y);
                    case 18: case 26: return x < y;
                    case 19: case 27: return std::int32_t(x) <= std::int32_t(y);
                    case 20: case 28: return x <= y;
                    default: return x == y;
                    }
                };
                switch (d.op[lane]) {
                case 0: value = x + y; carry = value < x; break;
                case 1: value = x + y + carry; break;
                case 2: value = x - y; carry = value > x; break;
                case 3: value = x - y - carry; break;
                case 4: case 6: {
                    const std::uint64_t a = d.op[lane] == 4 ? std::uint64_t(std::int64_t(std::int32_t(x))) : x;
                    const std::uint64_t b = d.op[lane] == 4 ? std::uint64_t(std::int64_t(std::int32_t(y))) : y;
                    const auto product = a * b;
                    value = Word(product); carry = Word(product >> 32); break;
                }
                case 5: value = carry; break;
                case 7:
                    // EXTENDED: x = 0x1000 is the load barrier (Mesa mme_tu104_load_barrier),
                    // nothing to do in a sequential interpreter.
                    if (x != 0x1000) fail("unsupported extended operation");
                    break;
                case 8: value = Word(std::countl_zero(x)); break;
                case 9: value = x << (y & 31); break;
                case 10: value = x >> (y & 31); break;
                case 11: value = Word(std::int32_t(x) >> (y & 31)); break;
                case 12: value = x & y; break;
                case 13: value = ~(x & y); break;
                case 14: value = x | y; break;
                case 15: value = x ^ y; break;
                case 16: {
                    const Word dest = (imm >> 10) & 63, count = (imm >> 5) & 31;
                    const Word source = imm & 31;
                    if (dest >= 32) { fail("invalid merge destination"); break; }
                    const Word bits_mask = (1U << count) - 1;
                    value = (x & ~(bits_mask << dest)) | (((y >> source) & bits_mask) << dest);
                    break;
                }
                case 17: case 18: case 19: case 20: case 21:
                    value = condition(d.op[lane]) ? ~0U : 0; break;
                case 22: value = read((x + y) & 0xffff); read_registers = true; break;
                case 23:
                    if (loop_count || imm < 2) { fail("invalid loop"); break; }
                    // The native SDK emits zero-count loops around FIFO reads.
                    // Unlike Mesa's test simulator, these must skip the body.
                    if (x == 0) { next_pc = pc + imm; break; }
                    loop_count = x - 1; loop_start = pc; loop_end = pc + imm - 1;
                    break;
                case 24: next_pc = pc + (imm & 0x7fff); break;
                case 25: case 26: case 27: case 28: case 29:
                    if (condition(d.op[lane]) == bool(imm & 0x8000)) {
                        const auto offset = std::int32_t(imm << 19) >> 19;
                        if (offset == -4096) stopped = true;
                        else {
                            next_pc = Word(std::int32_t(pc) + offset);
                            // Native back edges use bit 14 to suppress the
                            // fallthrough termination when the branch is taken.
                            branch_cancels_end = (imm & 0x4000) != 0;
                        }
                    }
                    break;
                case 30:
                    if (x >= active_dram->size()) { fail("data RAM read out of bounds"); break; }
                    value = (*active_dram)[x];
                    if (std::ranges::find(data_ram_writes, x, &DataRamAccess::first) != data_ram_writes.end()) {
                        reread_data_ram = true;
                    }
                    data_ram_reads.emplace_back(x, value);
                    break;
                case 31:
                    if (x >= active_dram->size()) { fail("data RAM write out of bounds"); break; }
                    (*active_dram)[x] = y;
                    data_ram_writes.emplace_back(x, y);
                    break;
                }
                alu[lane] = value;
                if (d.dst[lane] < 24) { regs[d.dst[lane]] = value; initialized |= 1U << d.dst[lane]; }
                else if (d.dst[lane] != 24) fail("invalid destination register");
            }
            const auto output = [&](Word selector) -> Word {
                switch (selector) {
                case 1: return alu[0]; case 2: return alu[1];
                case 3: return load[0]; case 4: return load[1];
                case 5: return d.imm[0]; case 6: return d.imm[1];
                case 8: return d.imm[0] >> 12; case 9: return d.imm[1] >> 12;
                case 10: return (d.imm[0] << 16) | d.imm[1];
                default: fail("invalid output selector"); return 0;
                }
            };
            for (Word lane = 0; lane < 2 && error.empty(); ++lane) {
                if (!(mask & (4U << lane))) continue;
                if (d.out_method[lane]) {
                    const Word address = output(d.out_method[lane]);
                    method = address & 0xfff; increment = (address >> 12) & 15;
                }
                if (d.out_emit[lane]) {
                    const Word value = output(d.out_emit[lane]);
                    if (error.empty() && !emit(method, value)) fail("unsupported emitted method");
                    method += increment;
                }
            }
            if (!error.empty()) break;
            if (end_next || stopped) return {true, pc, consumed, {}};
            end_next = d.end && !branch_cancels_end;
            if (loop_count && pc == loop_end) { --loop_count; next_pc = loop_start + 1; }
            pc = next_pc;
        }
        if (error.empty()) error = "instruction limit exceeded";
        return {false, pc, consumed, error};
    }

    using DataRam = std::array<Word, 0xc00>;
    DataRam dram{};
    // The data RAM DREAD/DWRITE use: a channel's own copy when the caller sets one. Macro code is
    // shared by the channels of a process, but each channel keeps its own data RAM (title 1's
    // channels store their own QMD ring base in word 0x27).
    DataRam* active_dram{&dram};
    // Decoded instructions by PC, re-decoded whenever their words differ.
    std::vector<Decoded> decoded;
    // What the last Execute read besides its parameters, for the macro memo: whether it read a
    // register (MME_READ), the data RAM words it read and wrote (address, value) in order, and
    // whether it read a word it had written itself.
    using DataRamAccess = std::pair<Word, Word>;
    bool read_registers{};
    bool reread_data_ram{};
    std::vector<DataRamAccess> data_ram_reads;
    std::vector<DataRamAccess> data_ram_writes;
};

} // namespace Tegra
