// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/macro_turing.h"

// Nx2Perf dma_forward: can a native (Turing) macro call run on the split front without the words
// its MME_DMA_READ_FIFOED reads? Analyze dry-runs the call with placeholder words and tracks
// which values derive from them (taint). The call qualifies when no tainted value reaches a
// branch, loop count, state read address, data RAM, method address or output enable, and the
// tainted values it emits are its last writes, to methods allowed to wait. Then which writes it
// makes, in which order, and every untainted value are the same for any words: the front sends
// the untainted writes, and the back runs Recompute in ring order, after the read's flushes, to
// write the tainted ones from the real words. Decided per call: the path depends on the call's
// parameters and the registers it reads.
namespace VideoCommon::DmaForward {

inline constexpr size_t MaxWrites = 64;
inline constexpr size_t MaxCode = 32;      ///< Instructions in the executed range
inline constexpr size_t MaxParameters = 8; ///< Before the DMA words
inline constexpr size_t MaxReads = 8;      ///< Register reads, and data RAM reads
inline constexpr size_t MaxRamWrites = 16;
inline constexpr size_t MaxDmaWords = 32;

/// A (register or data RAM address, value) pair; not std::pair, which isn't trivially copyable.
struct Recorded {
    u32 first;
    u32 second;
};

/// What the back needs to run the call again with the real words: trivially copyable, so it
/// travels in a ring entry. The code is a copy: the front may upload new code before the back
/// gets here.
struct Snapshot {
    u32 base_pc;
    u32 start;
    u32 num_code_words;
    u32 num_parameters;
    u32 num_state_reads;
    u32 num_ram_reads;
    u32 dma_words;
    u32 num_writes;
    u32 first_forwarded; ///< Writes from here on are the tainted ones
    GPUVAddr dma_source;
    std::array<u32, MaxCode * 3> code;
    std::array<u32, MaxParameters> parameters;
    std::array<Recorded, MaxReads> state_reads; ///< (register, value)
    std::array<Recorded, MaxReads> ram_reads;   ///< (address, value)
};
static_assert(std::is_trivially_copyable_v<Snapshot>);

struct Write {
    u32 method;
    u32 value; ///< Meaningless when forwarded
    bool forwarded;
};

struct Plan {
    bool eligible{};
    std::string_view reason;
    std::array<Write, MaxWrites> writes{};
    size_t num_writes{};
    size_t dma_write_index{}; ///< Position of the MME_DMA_READ_FIFOED write in `writes`
    std::array<std::pair<u32, u32>, MaxRamWrites> ram_writes{};
    size_t num_ram_writes{};
    Snapshot snapshot{};
};

/// Dry run of one call. `read(address, unknown)` returns the front's value of a 3D register and
/// sets `unknown` when the front doesn't know it (a forwarded write the back hasn't confirmed).
/// `forwardable(method)` says whether a value from the DMA words may be written to that method.
template <typename Read, typename Forwardable>
Plan Analyze(std::span<const u32> code, u32 start, std::span<const u32> parameters,
             std::span<const u32> data_ram, u32 macro_registers_start, Read&& read,
             Forwardable&& forwardable) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    Plan plan;
    Snapshot& snapshot = plan.snapshot;
    const auto fail = [&](std::string_view why) {
        if (plan.reason.empty()) {
            plan.reason = why;
        }
    };
    if (parameters.size() > MaxParameters) {
        plan.reason = "too many parameters";
        return plan;
    }
    std::copy(parameters.begin(), parameters.end(), snapshot.parameters.begin());
    snapshot.num_parameters = static_cast<u32>(parameters.size());
    // The load FIFO: the parameters, then (tainted) the DMA words once read.
    std::array<u32, MaxParameters + MaxDmaWords> fifo{};
    std::array<bool, MaxParameters + MaxDmaWords> fifo_taint{};
    std::copy(parameters.begin(), parameters.end(), fifo.begin());
    size_t fifo_size = parameters.size();
    size_t consumed{};
    bool dma_done{};
    bool forwarding{};
    std::array<u32, 24> regs{};
    std::array<bool, 24> taint{};
    u32 initialized{}, carry{}, method{}, increment{};
    bool carry_taint{};
    u32 pc = start, loop_count{}, loop_start{}, loop_end{};
    u32 min_pc = start, max_pc = start;
    bool end_next{}, stopped{};
    const auto ram_read = [&](u32 address) -> u32 {
        for (size_t i = plan.num_ram_writes; i-- > 0;) {
            if (plan.ram_writes[i].first == address) {
                return plan.ram_writes[i].second;
            }
        }
        const u32 value = data_ram[address];
        for (u32 i = 0; i < snapshot.num_ram_reads; ++i) {
            if (snapshot.ram_reads[i].first == address) {
                return value;
            }
        }
        if (snapshot.num_ram_reads == MaxReads) {
            fail("too many data RAM reads");
        } else {
            snapshot.ram_reads[snapshot.num_ram_reads++] = {address, value};
        }
        return value;
    };
    const auto state_read = [&](u32 address) -> u32 {
        u32 value{};
        bool written = false;
        for (size_t i = plan.num_writes; i-- > 0;) {
            if (plan.writes[i].method == address) {
                if (plan.writes[i].forwarded) {
                    fail("state read of a forwarded write");
                }
                value = plan.writes[i].value;
                written = true;
                break;
            }
        }
        if (!written) {
            bool unknown = false;
            value = read(address, unknown);
            if (unknown) {
                fail("state read of a register the front doesn't know");
            }
        }
        // Recompute answers every read from this list, in order (a register read twice with a
        // write between is recorded twice; the first match wins, so refuse that case).
        for (u32 i = 0; i < snapshot.num_state_reads; ++i) {
            if (snapshot.state_reads[i].first == address && snapshot.state_reads[i].second != value) {
                fail("a register read twice with different values");
            }
        }
        if (snapshot.num_state_reads == MaxReads) {
            fail("too many state reads");
        } else {
            snapshot.state_reads[snapshot.num_state_reads++] = {address, value};
        }
        return value;
    };
    for (u32 steps = 0; steps < 100000 && plan.reason.empty(); ++steps) {
        if (static_cast<size_t>(pc) * 3 + 2 >= code.size()) {
            fail("instruction outside uploaded RAM");
            break;
        }
        min_pc = (std::min)(min_pc, pc);
        max_pc = (std::max)(max_pc, pc);
        const auto d = Interpreter::Decode({code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
        std::array<u32, 2> load{}, alu{};
        std::array<bool, 2> load_taint{}, alu_taint{};
        for (u32 i = 0; i < 2; ++i) {
            if (d.load[i]) {
                if (consumed >= fifo_size) {
                    fail("parameter FIFO underflow");
                    break;
                }
                load_taint[i] = fifo_taint[consumed];
                load[i] = fifo[consumed++];
            }
        }
        if (!plan.reason.empty()) {
            break;
        }
        const auto get = [&](u32 reg, u32 lane, bool& t) -> u32 {
            t = false;
            if (reg < 24) {
                if (!(initialized & (1U << reg))) {
                    fail("uninitialized register read");
                }
                t = taint[reg];
                return regs[reg];
            }
            switch (reg) {
            case 24: return 0;
            case 25: return u32(s32(s16(d.imm[lane])));
            case 26: return u32(s32(s16(d.imm[1 - lane])));
            case 27: return (d.imm[0] << 16) | d.imm[1];
            case 28: t = load_taint[0]; return load[0];
            case 29: t = load_taint[1]; return load[1];
            default: fail("invalid source register"); return 0;
            }
        };
        // A predicate from the DMA words may only choose whether an ALU lane runs (its
        // destination then derives from them); never an output, a branch or a data RAM write.
        u32 mask = 15;
        u32 maybe_lanes = 0;
        if (d.pred_mode) {
            bool t{};
            const u32 predicate = get(d.pred_reg, 0, t);
            if (!t) {
                mask = predicate ? d.mask_true : d.mask_false;
            } else {
                if ((d.mask_true & 12) != (d.mask_false & 12)) {
                    fail("output enabled by DMA words");
                    break;
                }
                mask = d.mask_true | d.mask_false;
                maybe_lanes = (d.mask_true ^ d.mask_false) & 3;
            }
        }
        std::array<u32, 2> xs{}, ys{};
        std::array<bool, 2> xt{}, yt{};
        for (u32 lane = 0; lane < 2; ++lane) {
            if (mask & (1U << lane)) {
                xs[lane] = get(d.src_a[lane], lane, xt[lane]);
                ys[lane] = get(d.src_b[lane], lane, yt[lane]);
            }
        }
        u32 next_pc = pc + 1;
        bool branch_cancels_end{};
        for (u32 lane = 0; lane < 2 && plan.reason.empty(); ++lane) {
            if (!(mask & (1U << lane))) {
                continue;
            }
            const bool maybe = (maybe_lanes & (1U << lane)) != 0;
            const u32 op = d.op[lane];
            if (maybe && (op == 7 || op >= 22)) {
                fail("flow, state or data RAM operation predicated on DMA words");
                break;
            }
            const u32 x = xs[lane], y = ys[lane];
            const u32 imm = d.imm[lane];
            bool value_taint = xt[lane] || yt[lane] || maybe;
            u32 value{};
            const auto condition = [&](u32 comparison) {
                switch (comparison) {
                case 17: case 25: return s32(x) < s32(y);
                case 18: case 26: return x < y;
                case 19: case 27: return s32(x) <= s32(y);
                case 20: case 28: return x <= y;
                default: return x == y;
                }
            };
            switch (op) {
            case 0: value = x + y; carry = value < x; carry_taint = value_taint; break;
            case 1: value = x + y + carry; value_taint |= carry_taint; break;
            case 2: value = x - y; carry = value > x; carry_taint = value_taint; break;
            case 3: value = x - y - carry; value_taint |= carry_taint; break;
            case 4: case 6: {
                const u64 a = op == 4 ? u64(s64(s32(x))) : x;
                const u64 b = op == 4 ? u64(s64(s32(y))) : y;
                const u64 product = a * b;
                value = u32(product);
                carry = u32(product >> 32);
                carry_taint = value_taint;
                break;
            }
            case 5: value = carry; value_taint = carry_taint || maybe; break;
            case 7:
                value_taint = false;
                if (xt[lane] || x != 0x1000) fail("unsupported extended operation");
                break;
            case 8: value = u32(std::countl_zero(x)); break;
            case 9: value = x << (y & 31); break;
            case 10: value = x >> (y & 31); break;
            case 11: value = u32(s32(x) >> (y & 31)); break;
            case 12: value = x & y; break;
            case 13: value = ~(x & y); break;
            case 14: value = x | y; break;
            case 15: value = x ^ y; break;
            case 16: {
                const u32 dest = (imm >> 10) & 63, count = (imm >> 5) & 31;
                const u32 source = imm & 31;
                if (dest >= 32) { fail("invalid merge destination"); break; }
                const u32 bits_mask = (1U << count) - 1;
                value = (x & ~(bits_mask << dest)) | (((y >> source) & bits_mask) << dest);
                break;
            }
            case 17: case 18: case 19: case 20: case 21:
                value = condition(op) ? ~0U : 0;
                break;
            case 22:
                if (xt[lane] || yt[lane]) { fail("state read address from DMA words"); break; }
                value = state_read((x + y) & 0xffff);
                value_taint = false;
                break;
            case 23:
                value_taint = false;
                if (xt[lane]) { fail("loop count from DMA words"); break; }
                if (loop_count || imm < 2) { fail("invalid loop"); break; }
                if (x == 0) { next_pc = pc + imm; break; }
                loop_count = x - 1; loop_start = pc; loop_end = pc + imm - 1;
                break;
            case 24:
                value_taint = false;
                next_pc = pc + (imm & 0x7fff);
                break;
            case 25: case 26: case 27: case 28: case 29:
                value_taint = false;
                if (xt[lane] || yt[lane]) { fail("branch on DMA words"); break; }
                if (condition(op) == bool(imm & 0x8000)) {
                    const auto offset = s32(imm << 19) >> 19;
                    if (offset == -4096) {
                        stopped = true;
                    } else {
                        next_pc = u32(s32(pc) + offset);
                        branch_cancels_end = (imm & 0x4000) != 0;
                    }
                }
                break;
            case 30:
                value_taint = false; // the data RAM never holds a tainted value (case 31)
                if (xt[lane]) { fail("data RAM address from DMA words"); break; }
                if (x >= data_ram.size()) { fail("data RAM read out of bounds"); break; }
                value = ram_read(x);
                break;
            case 31:
                value_taint = false;
                if (xt[lane] || yt[lane]) { fail("data RAM write from DMA words"); break; }
                if (x >= data_ram.size()) { fail("data RAM write out of bounds"); break; }
                if (plan.num_ram_writes == MaxRamWrites) { fail("too many data RAM writes"); break; }
                plan.ram_writes[plan.num_ram_writes++] = {x, y};
                break;
            }
            alu[lane] = value;
            alu_taint[lane] = value_taint;
            if (d.dst[lane] < 24) {
                regs[d.dst[lane]] = value;
                taint[d.dst[lane]] = value_taint;
                initialized |= 1U << d.dst[lane];
            } else if (d.dst[lane] != 24) {
                fail("invalid destination register");
            }
        }
        if (!plan.reason.empty()) {
            break;
        }
        const auto output = [&](u32 selector, bool& t) -> u32 {
            t = false;
            switch (selector) {
            case 1: t = alu_taint[0]; return alu[0];
            case 2: t = alu_taint[1]; return alu[1];
            case 3: t = load_taint[0]; return load[0];
            case 4: t = load_taint[1]; return load[1];
            case 5: return d.imm[0];
            case 6: return d.imm[1];
            case 8: return d.imm[0] >> 12;
            case 9: return d.imm[1] >> 12;
            case 10: return (d.imm[0] << 16) | d.imm[1];
            default: fail("invalid output selector"); return 0;
            }
        };
        for (u32 lane = 0; lane < 2 && plan.reason.empty(); ++lane) {
            if (!(mask & (4U << lane))) {
                continue;
            }
            if (d.out_method[lane]) {
                bool t{};
                const u32 address = output(d.out_method[lane], t);
                if (t) { fail("method address from DMA words"); break; }
                method = address & 0xfff;
                increment = (address >> 12) & 15;
            }
            if (d.out_emit[lane]) {
                bool t{};
                const u32 value = output(d.out_emit[lane], t);
                if (method >= macro_registers_start) { fail("macro method write"); break; }
                if (plan.num_writes == MaxWrites) { fail("too many writes"); break; }
                if (t) {
                    if (!forwardable(method)) { fail("DMA words written to a method that can't wait"); break; }
                    if (!forwarding) {
                        forwarding = true;
                        snapshot.first_forwarded = static_cast<u32>(plan.num_writes);
                    }
                } else if (forwarding) {
                    fail("a write after a forwarded write");
                    break;
                }
                plan.writes[plan.num_writes++] = {method, value, t};
                if (!t && method == 0x157) {
                    fail("MME DMA read into data RAM");
                    break;
                }
                if (!t && method == 0x158) {
                    // MME_DMA_READ_FIFOED: the words follow in the load FIFO, tainted.
                    if (dma_done) { fail("more than one MME DMA read"); break; }
                    dma_done = true;
                    const u32 hi = state_read(0x154);
                    const u32 lo = state_read(0x155);
                    if (value == 0 || value > MaxDmaWords) { fail("DMA read too long"); break; }
                    snapshot.dma_source = (GPUVAddr{hi & 0x1ffffff} << 32) | lo;
                    snapshot.dma_words = value;
                    plan.dma_write_index = plan.num_writes - 1;
                    for (u32 i = 0; i < value; ++i) {
                        fifo[fifo_size] = 0;
                        fifo_taint[fifo_size++] = true;
                    }
                }
                method += increment;
            }
        }
        if (!plan.reason.empty()) {
            break;
        }
        if (end_next || stopped) {
            if (!dma_done || !forwarding) {
                plan.reason = !dma_done ? "no MME DMA read" : "nothing to forward";
                return plan;
            }
            if (max_pc - min_pc + 1 > MaxCode) {
                plan.reason = "too much code";
                return plan;
            }
            snapshot.base_pc = min_pc;
            snapshot.start = start - min_pc;
            snapshot.num_code_words = (max_pc - min_pc + 1) * 3;
            std::copy_n(code.begin() + size_t{min_pc} * 3, snapshot.num_code_words,
                        snapshot.code.begin());
            snapshot.num_writes = static_cast<u32>(plan.num_writes);
            plan.eligible = true;
            return plan;
        }
        end_next = d.end && !branch_cancels_end;
        if (loop_count && pc == loop_end) {
            --loop_count;
            next_pc = loop_start + 1;
        }
        pc = next_pc;
    }
    fail("instruction limit exceeded");
    return plan;
}

/// Runs a snapshot again with the real DMA words, as the drained front would have, and calls
/// `apply(method, value)` for each forwarded write. False when the run fails or makes a different
/// number of writes than the dry run (which would mean the analysis is wrong).
template <typename Apply>
bool Recompute(const Snapshot& snapshot, std::span<const u32> words, u32 macro_registers_start,
               Apply&& apply) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    thread_local std::unique_ptr<Interpreter> interpreter;
    thread_local std::vector<u32> fifo;
    if (!interpreter) {
        interpreter = std::make_unique<Interpreter>();
    }
    for (u32 i = 0; i < snapshot.num_ram_reads; ++i) {
        const auto [address, value] = snapshot.ram_reads[i];
        if (address < interpreter->dram.size()) {
            interpreter->dram[address] = value;
        }
    }
    interpreter->active_dram = &interpreter->dram;
    fifo.assign(snapshot.parameters.begin(),
                snapshot.parameters.begin() + snapshot.num_parameters);
    u32 index{};
    const auto result = interpreter->Execute(
        std::span<const u32>{snapshot.code.data(), snapshot.num_code_words}, snapshot.start, fifo,
        [&snapshot](u32 address) {
            for (u32 i = 0; i < snapshot.num_state_reads; ++i) {
                if (snapshot.state_reads[i].first == address) {
                    return snapshot.state_reads[i].second;
                }
            }
            return 0U;
        },
        [&](u32 method, u32 value) {
            if (method == 0x158) {
                fifo.insert(fifo.end(), words.begin(),
                            words.begin() + (std::min<size_t>)(value, words.size()));
            }
            if (index++ >= snapshot.first_forwarded) {
                apply(method, value);
            }
            return method < macro_registers_start;
        });
    return result.success && index == snapshot.num_writes;
}

} // namespace VideoCommon::DmaForward
