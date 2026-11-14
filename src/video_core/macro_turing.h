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
#include <unordered_map>
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

    template <typename Read, typename Emit>
    // `parameters` is the load FIFO; `emit` may append to it (MME_DMA_READ_FIFOED).
    Result Execute(std::span<const Word> code, Word start,
                   std::vector<Word>& parameters, Read read, Emit emit) {
        std::array<Word, 24> regs{};
        Word initialized{}, carry{}, method{}, increment{};
        Word pc = start, loop_count{}, loop_start{}, loop_end{};
        std::size_t consumed{};
        bool end_next{}, stopped{};
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

    /// What a call from `start` can do on any path through the code (each branch either way,
    /// each loop any number of times), as Execute runs it: the methods it can write, each a
    /// constant of the code unless `methods_known` is false, and whether it uses the data RAM.
    struct Effects {
        std::vector<Word> methods; ///< Ascending, each once
        bool methods_known{true};
        bool data_ram{};
        std::vector<Word> instructions; ///< The PCs a call can run, ascending
    };

    static Effects AnalyzeEffects(std::span<const Word> code, Word start) {
        // The values the code fixes, before an instruction runs: registers, carry, the method
        // and its increment. Unknown values are always {false, 0}.
        struct Value {
            bool known;
            Word value;
            bool operator==(const Value&) const = default;
        };
        struct State {
            std::array<Value, 24> regs;
            Value carry, method, increment;
            bool operator==(const State&) const = default;
        };
        // Where an instruction may continue: unchanged, at `pc` (a taken branch also sets
        // whether it cancels an END), or nowhere (a branch that stops the call).
        struct Flow {
            enum Kind { Next, Jump, Branch, Stop } kind;
            Word pc;
            bool cancel_end;
        };
        constexpr Value unknown{false, 0};
        const auto known = [](Word value) { return Value{true, value}; };
        const Word count = static_cast<Word>(code.size() / 3);
        Effects effects;
        // By instruction, and whether an END before it makes it the call's last (pc * 2 + 1).
        std::unordered_map<Word, State> states;
        // By a loop's last instruction: where its loops start over.
        std::unordered_map<Word, std::vector<Word>> loop_starts;
        std::vector<Word> work;
        const auto reach = [&](Word pc, bool last, const State& state) {
            if (pc >= count) {
                return; // The call fails there
            }
            const Word index = pc * 2 + (last ? 1U : 0U);
            const auto [it, inserted] = states.try_emplace(index, state);
            if (!inserted) {
                State joined = it->second;
                const auto join = [](Value& to, const Value& from) {
                    if (to != from) to = Value{false, 0};
                };
                for (std::size_t i = 0; i < joined.regs.size(); ++i) {
                    join(joined.regs[i], state.regs[i]);
                }
                join(joined.carry, state.carry);
                join(joined.method, state.method);
                join(joined.increment, state.increment);
                if (joined == it->second) return;
                it->second = joined;
            }
            work.push_back(index);
        };
        State entry;
        entry.regs.fill(unknown); // Reading one before writing it fails the call
        entry.carry = entry.method = entry.increment = known(0);
        reach(start, false, entry);
        while (!work.empty()) {
            const Word index = work.back();
            work.pop_back();
            const Word pc = index / 2;
            const bool last = (index & 1) != 0;
            const State before = states.at(index);
            const Decoded d = Decode({code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
            const auto get = [&d](const State& state, Word reg, Word lane) -> Value {
                if (reg < 24) return state.regs[reg];
                switch (reg) {
                case 24: return Value{true, 0};
                case 25: return Value{true, Word(std::int32_t(std::int16_t(d.imm[lane])))};
                case 26: return Value{true, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))};
                case 27: return Value{true, (d.imm[0] << 16) | d.imm[1]};
                default: return Value{false, 0}; // FIFO loads
                }
            };
            // The lanes and outputs the predicate enables, both sets if its register is unknown.
            std::array<Word, 2> masks{15, 15};
            std::size_t mask_count = 1;
            if (d.pred_mode) {
                const Value predicate = get(before, d.pred_reg, 0);
                masks = {d.mask_true, d.mask_false};
                if (predicate.known) masks[0] = predicate.value ? d.mask_true : d.mask_false;
                else mask_count = 2;
            }
            for (std::size_t m = 0; m < mask_count; ++m) {
                const Word mask = masks[m];
                State state = before;
                std::array<Value, 2> xs{unknown, unknown}, ys{unknown, unknown};
                for (Word lane = 0; lane < 2; ++lane) {
                    if (mask & (1U << lane)) {
                        xs[lane] = get(before, d.src_a[lane], lane);
                        ys[lane] = get(before, d.src_b[lane], lane);
                    }
                }
                std::array<Value, 2> alu{known(0), known(0)};
                std::array<std::array<Flow, 2>, 2> flows{};
                std::array<std::size_t, 2> flow_counts{1, 1}; // Flow::Next by default
                for (Word lane = 0; lane < 2; ++lane) {
                    if (!(mask & (1U << lane))) continue;
                    const Value x = xs[lane], y = ys[lane];
                    const bool both = x.known && y.known;
                    const Word a = x.value, b = y.value, imm = d.imm[lane];
                    auto& flow = flows[lane];
                    auto& flow_count = flow_counts[lane];
                    const auto taken = [&](Word op) {
                        switch (op) {
                        case 17: case 25: return std::int32_t(a) < std::int32_t(b);
                        case 18: case 26: return a < b;
                        case 19: case 27: return std::int32_t(a) <= std::int32_t(b);
                        case 20: case 28: return a <= b;
                        default: return a == b;
                        }
                    };
                    Value value = known(0);
                    switch (d.op[lane]) {
                    case 0: case 2: {
                        const Word result = d.op[lane] == 0 ? a + b : a - b;
                        value = both ? known(result) : unknown;
                        const bool carry = d.op[lane] == 0 ? result < a : result > a;
                        state.carry = both ? known(carry) : unknown;
                        break;
                    }
                    case 1: case 3:
                        value = both && state.carry.known
                                    ? known(d.op[lane] == 1 ? a + b + state.carry.value
                                                            : a - b - state.carry.value)
                                    : unknown;
                        break;
                    case 4: case 6: {
                        const std::uint64_t product =
                            d.op[lane] == 4 ? std::uint64_t(std::int64_t(std::int32_t(a))) *
                                                  std::uint64_t(std::int64_t(std::int32_t(b)))
                                            : std::uint64_t{a} * b;
                        value = both ? known(Word(product)) : unknown;
                        state.carry = both ? known(Word(product >> 32)) : unknown;
                        break;
                    }
                    case 5: value = state.carry; break;
                    case 8: value = x.known ? known(Word(std::countl_zero(a))) : unknown; break;
                    case 9: value = both ? known(a << (b & 31)) : unknown; break;
                    case 10: value = both ? known(a >> (b & 31)) : unknown; break;
                    case 11:
                        value = both ? known(Word(std::int32_t(a) >> (b & 31))) : unknown;
                        break;
                    case 12: value = both ? known(a & b) : unknown; break;
                    case 13: value = both ? known(~(a & b)) : unknown; break;
                    case 14: value = both ? known(a | b) : unknown; break;
                    case 15: value = both ? known(a ^ b) : unknown; break;
                    case 16: {
                        const Word dest = (imm >> 10) & 63, bits = (imm >> 5) & 31;
                        const Word bits_mask = (1U << bits) - 1;
                        value = both && dest < 32
                                    ? known((a & ~(bits_mask << dest)) |
                                            (((b >> (imm & 31)) & bits_mask) << dest))
                                    : unknown;
                        break;
                    }
                    case 17: case 18: case 19: case 20: case 21:
                        value = both ? known(taken(d.op[lane]) ? ~0U : 0) : unknown;
                        break;
                    case 22: value = unknown; break; // A register
                    case 23:
                        if (imm < 2) break;
                        // Entered, or skipped with a count of 0. Its last instruction may start
                        // it over: reached already, that instruction now continues here too.
                        if (const Word end = pc + imm - 1; end < count) {
                            auto& starts = loop_starts[end];
                            if (std::ranges::find(starts, pc + 1) == starts.end()) {
                                starts.push_back(pc + 1);
                                for (const Word end_index : {end * 2, end * 2 + 1}) {
                                    if (states.contains(end_index)) work.push_back(end_index);
                                }
                            }
                        }
                        flow_count = 0;
                        if (!x.known || a != 0) flow[flow_count++] = {Flow::Next, 0, false};
                        if (!x.known || a == 0) flow[flow_count++] = {Flow::Jump, pc + imm, false};
                        break;
                    case 24: flow[0] = {Flow::Jump, pc + (imm & 0x7fff), false}; break;
                    case 25: case 26: case 27: case 28: case 29: {
                        const auto offset = std::int32_t(imm << 19) >> 19;
                        const Word target = Word(std::int32_t(pc) + offset);
                        const bool jumps = taken(d.op[lane]) == bool(imm & 0x8000);
                        flow_count = 0;
                        if (!both || jumps) {
                            const bool cancel_end = (imm & 0x4000) != 0;
                            flow[flow_count++] = offset == -4096
                                                     ? Flow{Flow::Stop, 0, false}
                                                     : Flow{Flow::Branch, target, cancel_end};
                        }
                        if (!both || !jumps) flow[flow_count++] = {Flow::Next, 0, false};
                        break;
                    }
                    case 30: effects.data_ram = true; value = unknown; break;
                    case 31: effects.data_ram = true; break;
                    default: break; // 7: the load barrier
                    }
                    alu[lane] = value;
                    if (d.dst[lane] < 24) state.regs[d.dst[lane]] = value;
                }
                const auto output = [&](Word selector) -> Value {
                    switch (selector) {
                    case 1: return alu[0];
                    case 2: return alu[1];
                    case 5: return known(d.imm[0]);
                    case 6: return known(d.imm[1]);
                    case 8: return known(d.imm[0] >> 12);
                    case 9: return known(d.imm[1] >> 12);
                    case 10: return known((d.imm[0] << 16) | d.imm[1]);
                    default: return unknown; // FIFO loads (the rest fail the call)
                    }
                };
                for (Word lane = 0; lane < 2; ++lane) {
                    if (!(mask & (4U << lane))) continue;
                    if (d.out_method[lane]) {
                        const Value address = output(d.out_method[lane]);
                        state.method = address.known ? known(address.value & 0xfff) : unknown;
                        state.increment =
                            address.known ? known((address.value >> 12) & 15) : unknown;
                    }
                    if (d.out_emit[lane]) {
                        if (state.method.known) effects.methods.push_back(state.method.value);
                        else effects.methods_known = false;
                        state.method = state.method.known && state.increment.known
                                           ? known(state.method.value + state.increment.value)
                                           : unknown;
                    }
                }
                if (last) continue; // An END before this instruction ends the call after it
                // Lane 1's control flow overrides lane 0's; a loop's last instruction may also
                // start it over.
                const auto loop = loop_starts.find(pc);
                const std::vector<Word> repeats =
                    loop != loop_starts.end() ? loop->second : std::vector<Word>{};
                for (std::size_t i = 0; i < flow_counts[0]; ++i) {
                    for (std::size_t j = 0; j < flow_counts[1]; ++j) {
                        Word next = pc + 1;
                        bool cancel_end = false, stopped = false;
                        for (const Flow& flow : {flows[0][i], flows[1][j]}) {
                            if (flow.kind == Flow::Jump || flow.kind == Flow::Branch) {
                                next = flow.pc;
                            }
                            if (flow.kind == Flow::Branch) cancel_end = flow.cancel_end;
                            stopped |= flow.kind == Flow::Stop;
                        }
                        if (stopped) continue;
                        const bool end_next = d.end && !cancel_end;
                        reach(next, end_next, state);
                        for (const Word repeat : repeats) reach(repeat, end_next, state);
                    }
                }
            }
        }
        std::ranges::sort(effects.methods);
        effects.methods.erase(std::unique(effects.methods.begin(), effects.methods.end()),
                              effects.methods.end());
        for (const auto& reached : states) {
            effects.instructions.push_back(reached.first / 2);
        }
        std::ranges::sort(effects.instructions);
        effects.instructions.erase(
            std::unique(effects.instructions.begin(), effects.instructions.end()),
            effects.instructions.end());
        return effects;
    }

    /// A macro's identity wherever its code was uploaded: the instructions a call from `start`
    /// can run (AnalyzeEffects), each by its place after `start` (FNV-1a).
    static std::uint64_t CodeHash(std::span<const Word> code, Word start, const Effects& effects) {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        const auto add = [&hash](Word word) {
            for (Word shift = 0; shift < 32; shift += 8) {
                hash = (hash ^ ((word >> shift) & 0xff)) * 0x100000001b3ULL;
            }
        };
        for (const Word pc : effects.instructions) {
            add(pc - start);
            add(code[pc * 3]);
            add(code[pc * 3 + 1]);
            add(code[pc * 3 + 2]);
        }
        return hash;
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
