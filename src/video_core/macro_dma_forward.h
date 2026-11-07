// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
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

/// A Snapshot without its code (dma_forward_memo: the code lives in a shared slot).
struct SlimSnapshot {
    u32 base_pc;
    u32 start;
    u32 num_code_words;
    u32 num_parameters;
    u32 num_state_reads;
    u32 num_ram_reads;
    u32 dma_words;
    u32 num_writes;
    u32 first_forwarded; ///< Writes from here on are the tainted ones
    u32 unused;          ///< No padding: verify compares snapshots bytewise
    GPUVAddr dma_source;
    std::array<u32, MaxParameters> parameters;
    std::array<Recorded, MaxReads> state_reads; ///< (register, value)
    std::array<Recorded, MaxReads> ram_reads;   ///< (address, value)
};
static_assert(std::is_trivially_copyable_v<SlimSnapshot>);
static_assert(sizeof(SlimSnapshot) == 0xd0 && std::has_unique_object_representations_v<SlimSnapshot>);

/// What the back needs to run the call again with the real words: trivially copyable, so it
/// travels in a ring entry. The code is a copy: the front may upload new code before the back
/// gets here.
struct Snapshot : SlimSnapshot {
    std::array<u32, MaxCode * 3> code;
};
static_assert(std::is_trivially_copyable_v<Snapshot>);

struct Write {
    u32 method;
    u32 value; ///< Meaningless when forwarded
    bool forwarded;
};

// Nx2Perf dma_forward_tape: the forwarded values as straight-line code over the DMA words.
// Slots 0..MaxDmaWords-1 hold the words, op i writes slot MaxDmaWords + i.
inline constexpr size_t MaxTapeOps = 8;
inline constexpr size_t MaxTapeWrites = 4;
inline constexpr size_t TapeSlots = MaxDmaWords + MaxTapeOps;
inline constexpr u8 TapeImm = 0xff; ///< Operand `a` or `b`: the op's `imm`

enum class TapeOpcode : u8 {
    Const, ///< imm
    Add,
    AddCarry,  ///< (a + b) < a
    Sub,
    SubBorrow, ///< (a - b) > a
    MulLo,
    MulHiS,
    MulHiU,
    Clz,
    Shl,
    Shr,
    Sar,
    And,
    Nand,
    Or,
    Xor,
    Merge, ///< imm: the merge immediate; a and b are slots
    Lt,
    Ltu,
    Le,
    Leu,
    Eq,
    Select, ///< imm: predicate slot; a when it is non-zero, else b (both slots)
};

struct TapeOp {
    u8 op;
    u8 dst;
    u8 a;
    u8 b;
    u32 imm;
};
static_assert(sizeof(TapeOp) == 8 && std::has_unique_object_representations_v<TapeOp>);

struct TapeWrite {
    u16 method;
    u8 slot;
    u8 unused;
};
static_assert(sizeof(TapeWrite) == 4 && std::has_unique_object_representations_v<TapeWrite>);

struct Tape {
    u8 dma_words;
    u8 num_ops;
    u8 num_writes;
    u8 unused;
    std::array<TapeOp, MaxTapeOps> ops;
    std::array<TapeWrite, MaxTapeWrites> writes;
};
static_assert(sizeof(Tape) == 0x54 && std::is_trivially_copyable_v<Tape> &&
              std::has_unique_object_representations_v<Tape>);

enum class TapeFallback : u8 { None, Overflow, Unsupported, NotRecorded };

/// One tape op on values (Const and Select are handled by the callers).
constexpr u32 EvalTapeOp(TapeOpcode op, u32 a, u32 b, u32 imm) {
    switch (op) {
    case TapeOpcode::Add: return a + b;
    case TapeOpcode::AddCarry: return (a + b) < a ? 1U : 0U;
    case TapeOpcode::Sub: return a - b;
    case TapeOpcode::SubBorrow: return (a - b) > a ? 1U : 0U;
    case TapeOpcode::MulLo: return a * b;
    case TapeOpcode::MulHiS:
        return u32((u64(s64(s32(a))) * u64(s64(s32(b)))) >> 32);
    case TapeOpcode::MulHiU: return u32((u64{a} * u64{b}) >> 32);
    case TapeOpcode::Clz: return u32(std::countl_zero(a));
    case TapeOpcode::Shl: return a << (b & 31);
    case TapeOpcode::Shr: return a >> (b & 31);
    case TapeOpcode::Sar: return u32(s32(a) >> (b & 31));
    case TapeOpcode::And: return a & b;
    case TapeOpcode::Nand: return ~(a & b);
    case TapeOpcode::Or: return a | b;
    case TapeOpcode::Xor: return a ^ b;
    case TapeOpcode::Merge: {
        const u32 dest = (imm >> 10) & 63, count = (imm >> 5) & 31, source = imm & 31;
        const u32 bits_mask = (1U << count) - 1;
        return (a & ~(bits_mask << dest)) | (((b >> source) & bits_mask) << dest);
    }
    case TapeOpcode::Lt: return s32(a) < s32(b) ? ~0U : 0U;
    case TapeOpcode::Ltu: return a < b ? ~0U : 0U;
    case TapeOpcode::Le: return s32(a) <= s32(b) ? ~0U : 0U;
    case TapeOpcode::Leu: return a <= b ? ~0U : 0U;
    case TapeOpcode::Eq: return a == b ? ~0U : 0U;
    case TapeOpcode::Const:
    case TapeOpcode::Select: break;
    }
    return 0;
}

/// Runs a tape over `slots` (the DMA words in its first tape.dma_words entries) and calls
/// `apply(method, value)` for each forwarded write, in order.
template <typename Apply>
void EvalTape(const Tape& tape, std::array<u32, TapeSlots>& slots, Apply&& apply) {
    for (u32 i = 0; i < tape.num_ops; ++i) {
        const TapeOp& op = tape.ops[i];
        const auto opcode = static_cast<TapeOpcode>(op.op);
        u32 value;
        if (opcode == TapeOpcode::Const) {
            value = op.imm;
        } else if (opcode == TapeOpcode::Select) {
            value = slots[op.imm] != 0 ? slots[op.a] : slots[op.b];
        } else {
            const u32 a = op.a == TapeImm ? op.imm : slots[op.a];
            const u32 b = op.b == TapeImm ? op.imm : slots[op.b];
            value = EvalTapeOp(opcode, a, b, op.imm);
        }
        slots[op.dst] = value;
    }
    for (u32 i = 0; i < tape.num_writes; ++i) {
        apply(u32{tape.writes[i].method}, slots[tape.writes[i].slot]);
    }
}

/// Builds a tape during Analyze's dry run: a value is a constant or a slot. Raw ops are numbered
/// from slot MaxDmaWords; Finish drops the dead ones and renumbers the rest.
class TapeRecorder {
public:
    static constexpr u8 ConstSlot = 0xff;
    static constexpr size_t MaxRawOps = 64;

    struct Val {
        u8 slot = ConstSlot;
        u32 value = 0; ///< When slot == ConstSlot
    };

    static constexpr Val Const(u32 value) {
        return {ConstSlot, value};
    }
    static constexpr Val Slot(u32 slot) {
        return {static_cast<u8>(slot), 0};
    }
    static constexpr bool IsConst(Val v) {
        return v.slot == ConstSlot;
    }
    static constexpr bool IsConst(Val v, u32 value) {
        return v.slot == ConstSlot && v.value == value;
    }

    bool Ok() const {
        return fallback == TapeFallback::None;
    }
    void Fail(TapeFallback why) {
        if (fallback == TapeFallback::None) {
            fallback = why;
        }
    }

    Val Bin(TapeOpcode op, Val a, Val b, u32 imm = 0) {
        if (IsConst(a) && IsConst(b)) {
            return Const(EvalTapeOp(op, a.value, b.value, imm));
        }
        // Exact identities only.
        switch (op) {
        case TapeOpcode::Add:
        case TapeOpcode::Or:
        case TapeOpcode::Xor:
            if (IsConst(b, 0)) return a;
            if (IsConst(a, 0)) return b;
            break;
        case TapeOpcode::Sub:
            if (IsConst(b, 0)) return a;
            break;
        case TapeOpcode::AddCarry:
            if (IsConst(a, 0) || IsConst(b, 0)) return Const(0);
            break;
        case TapeOpcode::SubBorrow:
            if (IsConst(b, 0)) return Const(0);
            break;
        case TapeOpcode::And:
            if (IsConst(b, ~0U)) return a;
            if (IsConst(a, ~0U)) return b;
            break;
        default:
            break;
        }
        if (op == TapeOpcode::Merge) {
            const u8 sa = ToSlot(a);
            const u8 sb = ToSlot(b);
            return Emit({static_cast<u8>(op), 0, sa, sb, imm});
        }
        // At most one constant here: it goes in imm.
        if (IsConst(a)) {
            return Emit({static_cast<u8>(op), 0, TapeImm, b.slot, a.value});
        }
        if (IsConst(b)) {
            return Emit({static_cast<u8>(op), 0, a.slot, TapeImm, b.value});
        }
        return Emit({static_cast<u8>(op), 0, a.slot, b.slot, 0});
    }

    /// `if_set` when the predicate is non-zero, else `if_clear`.
    Val Select(Val predicate, Val if_set, Val if_clear) {
        if (IsConst(predicate)) {
            return predicate.value != 0 ? if_set : if_clear;
        }
        if (if_set.slot == if_clear.slot && (!IsConst(if_set) || if_set.value == if_clear.value)) {
            return if_set;
        }
        const u8 sa = ToSlot(if_set);
        const u8 sb = ToSlot(if_clear);
        return Emit({static_cast<u8>(TapeOpcode::Select), 0, sa, sb, predicate.slot});
    }

    void Write(u32 method, Val v) {
        if (num_writes == MaxTapeWrites) {
            Fail(TapeFallback::Overflow);
            return;
        }
        writes[num_writes++] = {static_cast<u16>(method), ToSlot(v), 0};
    }

    /// The live ops, renumbered, into `tape`. False (and `fallback` set) when they don't fit.
    bool Finish(Tape& tape, u32 dma_words) {
        if (!Ok()) {
            return false;
        }
        std::array<bool, MaxRawOps> live{};
        const auto mark = [&](u32 slot) {
            if (slot >= MaxDmaWords && slot != TapeImm) {
                live[slot - MaxDmaWords] = true;
            }
        };
        for (u32 i = 0; i < num_writes; ++i) {
            mark(writes[i].slot);
        }
        for (u32 i = num_raw; i-- > 0;) {
            if (!live[i]) {
                continue;
            }
            const TapeOp& op = raw[i];
            const auto opcode = static_cast<TapeOpcode>(op.op);
            if (opcode == TapeOpcode::Const) {
                continue;
            }
            mark(op.a);
            mark(op.b);
            if (opcode == TapeOpcode::Select) {
                mark(op.imm);
            }
        }
        std::array<u8, MaxRawOps> renamed{};
        u32 count = 0;
        for (u32 i = 0; i < num_raw; ++i) {
            if (live[i]) {
                if (count == MaxTapeOps) {
                    Fail(TapeFallback::Overflow);
                    return false;
                }
                renamed[i] = static_cast<u8>(MaxDmaWords + count++);
            }
        }
        const auto rename = [&](u32 slot) -> u8 {
            return slot >= MaxDmaWords && slot != TapeImm ? renamed[slot - MaxDmaWords]
                                                          : static_cast<u8>(slot);
        };
        tape = {};
        tape.dma_words = static_cast<u8>(dma_words);
        for (u32 i = 0; i < num_raw; ++i) {
            if (!live[i]) {
                continue;
            }
            TapeOp op = raw[i];
            const auto opcode = static_cast<TapeOpcode>(op.op);
            op.dst = renamed[i];
            if (opcode != TapeOpcode::Const) {
                op.a = rename(op.a);
                op.b = rename(op.b);
                if (opcode == TapeOpcode::Select) {
                    op.imm = rename(op.imm);
                }
            }
            tape.ops[tape.num_ops++] = op;
        }
        for (u32 i = 0; i < num_writes; ++i) {
            tape.writes[i] = {writes[i].method, rename(writes[i].slot), 0};
        }
        tape.num_writes = static_cast<u8>(num_writes);
        return true;
    }

    TapeFallback fallback = TapeFallback::None;
    std::array<Val, 24> regs{}; ///< Valid for the registers Analyze marks tainted
    std::array<Val, 2> load{};
    std::array<Val, 2> alu{};
    Val carry{};
    u32 dma_fifo_base{}; ///< FIFO index of DMA word 0

private:
    u8 ToSlot(Val v) {
        if (!IsConst(v)) {
            return v.slot;
        }
        for (u32 i = 0; i < num_raw; ++i) {
            if (raw[i].op == static_cast<u8>(TapeOpcode::Const) && raw[i].imm == v.value) {
                return static_cast<u8>(MaxDmaWords + i);
            }
        }
        return Emit({static_cast<u8>(TapeOpcode::Const), 0, 0, 0, v.value}).slot;
    }

    Val Emit(TapeOp op) {
        if (num_raw == MaxRawOps) {
            Fail(TapeFallback::Overflow);
            return Const(0);
        }
        op.dst = static_cast<u8>(MaxDmaWords + num_raw);
        raw[num_raw++] = op;
        return Slot(op.dst);
    }

    std::array<TapeOp, MaxRawOps> raw{};
    u32 num_raw{};
    std::array<TapeWrite, MaxTapeWrites> writes{};
    u32 num_writes{};
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
    bool tape_valid{}; ///< dma_forward_tape: `tape` gives the forwarded writes
    TapeFallback tape_fallback{TapeFallback::NotRecorded};
    Tape tape{};
};

/// Dry run of one call. `read(address, unknown)` returns the front's value of a 3D register and
/// sets `unknown` when the front doesn't know it (a forwarded write the back hasn't confirmed).
/// `forwardable(method)` says whether a value from the DMA words may be written to that method.
/// RecordTape: also builds plan.tape (dma_forward_tape).
template <bool RecordTape = false, typename Read, typename Forwardable>
Plan Analyze(std::span<const u32> code, u32 start, std::span<const u32> parameters,
             std::span<const u32> data_ram, u32 macro_registers_start, Read&& read,
             Forwardable&& forwardable) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    Plan plan;
    Snapshot& snapshot = plan.snapshot;
    struct NoRecorder {};
    [[maybe_unused]] std::conditional_t<RecordTape, TapeRecorder, NoRecorder> rec;
    using Val = TapeRecorder::Val;
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
                if constexpr (RecordTape) {
                    if (load_taint[i]) {
                        rec.load[i] = TapeRecorder::Slot(static_cast<u32>(consumed) -
                                                         rec.dma_fifo_base);
                    }
                }
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
        [[maybe_unused]] Val predicate_val{};
        // The tape's value of a source Analyze marks tainted (registers and loads only).
        [[maybe_unused]] const auto tainted_val = [&](u32 reg) -> Val {
            if constexpr (RecordTape) {
                if (reg < 24) {
                    return rec.regs[reg];
                }
                if (reg == 28 || reg == 29) {
                    return rec.load[reg - 28];
                }
                rec.Fail(TapeFallback::Unsupported);
            }
            return {};
        };
        if (d.pred_mode) {
            bool t{};
            const u32 predicate = get(d.pred_reg, 0, t);
            if constexpr (RecordTape) {
                if (t) {
                    predicate_val = tainted_val(d.pred_reg);
                }
            }
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
        [[maybe_unused]] std::array<Val, 2> xv{}, yv{};
        for (u32 lane = 0; lane < 2; ++lane) {
            if (mask & (1U << lane)) {
                xs[lane] = get(d.src_a[lane], lane, xt[lane]);
                ys[lane] = get(d.src_b[lane], lane, yt[lane]);
                if constexpr (RecordTape) {
                    // Both lanes read their operands before either writes.
                    xv[lane] = xt[lane] ? tainted_val(d.src_a[lane]) : TapeRecorder::Const(xs[lane]);
                    yv[lane] = yt[lane] ? tainted_val(d.src_b[lane]) : TapeRecorder::Const(ys[lane]);
                }
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
            [[maybe_unused]] const u32 carry_before = carry;
            [[maybe_unused]] const bool carry_taint_before = carry_taint;
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
            if constexpr (RecordTape) {
                if (value_taint && plan.reason.empty()) {
                    using enum TapeOpcode;
                    const Val a = xv[lane], b = yv[lane];
                    const Val carry_old =
                        carry_taint_before ? rec.carry : TapeRecorder::Const(carry_before);
                    Val result{};
                    Val carry_new{};
                    bool writes_carry = false;
                    switch (op) {
                    case 0: result = rec.Bin(Add, a, b); carry_new = rec.Bin(AddCarry, a, b); writes_carry = true; break;
                    case 1: result = rec.Bin(Add, rec.Bin(Add, a, b), carry_old); break;
                    case 2: result = rec.Bin(Sub, a, b); carry_new = rec.Bin(SubBorrow, a, b); writes_carry = true; break;
                    case 3: result = rec.Bin(Sub, rec.Bin(Sub, a, b), carry_old); break;
                    case 4: case 6:
                        result = rec.Bin(MulLo, a, b);
                        carry_new = rec.Bin(op == 4 ? MulHiS : MulHiU, a, b);
                        writes_carry = true;
                        break;
                    case 5: result = carry_old; break;
                    case 8: result = rec.Bin(Clz, a, TapeRecorder::Const(0)); break;
                    case 9: result = rec.Bin(Shl, a, b); break;
                    case 10: result = rec.Bin(Shr, a, b); break;
                    case 11: result = rec.Bin(Sar, a, b); break;
                    case 12: result = rec.Bin(And, a, b); break;
                    case 13: result = rec.Bin(Nand, a, b); break;
                    case 14: result = rec.Bin(Or, a, b); break;
                    case 15: result = rec.Bin(Xor, a, b); break;
                    case 16: result = rec.Bin(Merge, a, b, imm); break;
                    case 17: result = rec.Bin(Lt, a, b); break;
                    case 18: result = rec.Bin(Ltu, a, b); break;
                    case 19: result = rec.Bin(Le, a, b); break;
                    case 20: result = rec.Bin(Leu, a, b); break;
                    case 21: result = rec.Bin(Eq, a, b); break;
                    default: rec.Fail(TapeFallback::Unsupported); break;
                    }
                    const u32 dst = d.dst[lane];
                    if (maybe) {
                        // Select(predicate, ran, skipped) by which mask enables the lane.
                        const bool runs_when_set = (d.mask_true & (1U << lane)) != 0;
                        const auto choose = [&](Val ran, Val skipped) {
                            return runs_when_set ? rec.Select(predicate_val, ran, skipped)
                                                 : rec.Select(predicate_val, skipped, ran);
                        };
                        if (writes_carry) {
                            carry_new = choose(carry_new, carry_old);
                        }
                        rec.alu[lane] = choose(result, TapeRecorder::Const(0));
                        if (dst < 24) {
                            if (!(initialized & (1U << dst))) {
                                rec.Fail(TapeFallback::Unsupported);
                            }
                            result = choose(result, taint[dst] ? rec.regs[dst]
                                                               : TapeRecorder::Const(regs[dst]));
                        }
                    } else {
                        rec.alu[lane] = result;
                    }
                    if (writes_carry) {
                        rec.carry = carry_new;
                    }
                    if (dst < 24) {
                        rec.regs[dst] = result;
                    }
                }
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
                if constexpr (RecordTape) {
                    if (t) {
                        const u32 selector = d.out_emit[lane];
                        if (selector == 1 || selector == 2) {
                            rec.Write(method, rec.alu[selector - 1]);
                        } else if (selector == 3 || selector == 4) {
                            rec.Write(method, rec.load[selector - 3]);
                        } else {
                            rec.Fail(TapeFallback::Unsupported);
                        }
                    }
                }
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
                    if constexpr (RecordTape) {
                        rec.dma_fifo_base = static_cast<u32>(fifo_size);
                    }
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
            if constexpr (RecordTape) {
                plan.tape_valid = rec.Finish(plan.tape, snapshot.dma_words);
                plan.tape_fallback = rec.fallback;
            }
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
bool Recompute(const SlimSnapshot& snapshot, std::span<const u32> code,
               std::span<const u32> words, u32 macro_registers_start, Apply&& apply) {
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
        code.first(snapshot.num_code_words), snapshot.start, fifo,
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

template <typename Apply>
bool Recompute(const Snapshot& snapshot, std::span<const u32> words, u32 macro_registers_start,
               Apply&& apply) {
    return Recompute(static_cast<const SlimSnapshot&>(snapshot),
                     std::span<const u32>{snapshot.code}, words, macro_registers_start,
                     std::forward<Apply>(apply));
}

/// Same writes, data RAM writes and snapshot (dma_forward_memo's verify).
inline bool SamePlan(const Plan& a, const Plan& b) {
    if (a.eligible != b.eligible || a.num_writes != b.num_writes ||
        a.dma_write_index != b.dma_write_index || a.num_ram_writes != b.num_ram_writes ||
        std::memcmp(static_cast<const SlimSnapshot*>(&a.snapshot),
                    static_cast<const SlimSnapshot*>(&b.snapshot), sizeof(SlimSnapshot)) != 0 ||
        std::memcmp(a.snapshot.code.data(), b.snapshot.code.data(),
                    size_t{a.snapshot.num_code_words} * sizeof(u32)) != 0 ||
        a.tape_valid != b.tape_valid || a.tape_fallback != b.tape_fallback ||
        std::memcmp(&a.tape, &b.tape, sizeof(Tape)) != 0) {
        return false;
    }
    for (size_t i = 0; i < a.num_writes; ++i) {
        if (a.writes[i].method != b.writes[i].method || a.writes[i].value != b.writes[i].value ||
            a.writes[i].forwarded != b.writes[i].forwarded) {
            return false;
        }
    }
    for (size_t i = 0; i < a.num_ram_writes; ++i) {
        if (a.ram_writes[i] != b.ram_writes[i]) {
            return false;
        }
    }
    return true;
}

/// Nx2Perf dma_forward_memo: front-owned. Analyze is a pure function of the code generation,
/// the start, the parameters, the registers it read through `read` (value and unknown flag) and
/// the data RAM words it read (the plan's ram_reads), so a plan whose inputs all match again is
/// reused. Code slots hold immutable copies of the executed code range for the ring entries; a
/// slot is reused only once the back has passed the last entry that named it.
struct RegisterRead {
    u32 address;
    u32 value;
    bool unknown;
};

struct MemoEntry {
    bool valid{};
    u64 generation{};
    u32 start{};
    u32 num_parameters{};
    std::array<u32, MaxParameters> parameters{};
    u32 num_reads{};
    std::array<RegisterRead, MaxReads> reads{};
    Plan plan{};
};

struct CodeSlot {
    bool valid{};
    u64 generation{};
    u32 base_pc{};
    u32 num_code_words{};
    u64 last_use{}; ///< Ring position after the last entry naming this slot
    std::array<u32, MaxCode * 3> code{};
};

inline constexpr size_t MemoEntries = 8;
inline constexpr size_t MemoCodeSlots = 8;

struct Memo {
    std::array<MemoEntry, MemoEntries> entries{};
    u32 next_victim{};
    std::array<CodeSlot, MemoCodeSlots> slots{};
    u64 hits{};
    u64 misses{};
    u64 slot_full{};
    u64 slim_entries{};
    u64 last_log_ns{};
    u32 calls_since_check{};

    /// The cached plan for these inputs, or null. `read` as Analyze's.
    template <typename Read>
    const MemoEntry* Find(u64 generation, u32 start, std::span<const u32> parameters,
                          std::span<const u32> data_ram, Read&& read) const {
        for (const MemoEntry& entry : entries) {
            if (!entry.valid || entry.generation != generation || entry.start != start ||
                entry.num_parameters != parameters.size() ||
                !std::equal(parameters.begin(), parameters.end(), entry.parameters.begin())) {
                continue;
            }
            bool same = true;
            for (u32 i = 0; same && i < entry.num_reads; ++i) {
                bool unknown = false;
                const u32 value = read(entry.reads[i].address, unknown);
                same = value == entry.reads[i].value && unknown == entry.reads[i].unknown;
            }
            const Snapshot& snapshot = entry.plan.snapshot;
            for (u32 i = 0; same && i < snapshot.num_ram_reads; ++i) {
                const auto [address, value] = snapshot.ram_reads[i];
                same = address < data_ram.size() && data_ram[address] == value;
            }
            if (same) {
                return &entry;
            }
        }
        return nullptr;
    }

    /// The entry to refill (round robin).
    MemoEntry& Victim() {
        MemoEntry& entry = entries[next_victim];
        next_victim = (next_victim + 1) % MemoEntries;
        return entry;
    }

    /// A slot holding the snapshot's code, or null when every slot is still in use by the back
    /// (`consumed`: the back's ring position).
    CodeSlot* AcquireSlot(u64 generation, const Snapshot& snapshot, u64 consumed) {
        CodeSlot* free = nullptr;
        for (CodeSlot& slot : slots) {
            if (slot.valid && slot.generation == generation && slot.base_pc == snapshot.base_pc &&
                slot.num_code_words == snapshot.num_code_words) {
                return &slot;
            }
            if (free == nullptr && (!slot.valid || slot.last_use <= consumed)) {
                free = &slot;
            }
        }
        if (free != nullptr) {
            free->valid = true;
            free->generation = generation;
            free->base_pc = snapshot.base_pc;
            free->num_code_words = snapshot.num_code_words;
            std::copy_n(snapshot.code.begin(), snapshot.num_code_words, free->code.begin());
        }
        return free;
    }
};

} // namespace VideoCommon::DmaForward
