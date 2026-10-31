// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <oaknut/oaknut.hpp>

#include "common/logging.h"
#include "video_core/macro_turing.h"

namespace Tegra {

// ARM64 code for the TU104/GA10x MME, the port of the x86-64 TuringMacroJit (macro_turing_jit.h)
// with the same interface and semantics: every instruction of the uploaded macro memory compiled
// once (each a block that falls through to the next one, branches and loops through a jump
// table), emits (method writes) and register reads calling back into C++.
//
// Nobody can step through this code on a phone, so it checks itself: after each compile, the
// first CheckedCalls calls of each entry point also run TuringMacroInterpreter, which replays what
// the compiled code saw (register reads, the FIFO words and data RAM words its emits added)
// instead of calling out, and every emit, read, the result and the data RAM are compared. A
// difference is logged once and the program goes back to the interpreter (Usable() is false).
class TuringMacroJit {
public:
    using Word = std::uint32_t;
    using Result = TuringMacroInterpreter::Result;

    struct State {
        std::array<Word, 24> regs;
        Word carry;
        Word method;
        Word increment;
        Word loop_count;
        Word loop_start;
        Word loop_end;
        Word next_pc;
        Word steps;
        Word start;
        Word consumed;
        Word params_size;
        Word fail_pc;
        std::uint8_t end_next;
        std::uint8_t stopped;
        std::uint8_t cancel_end;
        std::uint8_t pad;
        Word scratch_x;
        Word scratch_y;
        const Word* params;
        Word* dram;
        void* user;
        bool (*emit)(State*, Word method, Word value);
        Word (*read)(State*, Word address);
        const std::vector<Word>* fifo;
    };

    enum Error : Word {
        None = 0,
        FifoUnderflow,
        InvalidSource,
        InvalidDestination,
        UnsupportedExtended,
        InvalidMerge,
        InvalidLoop,
        DataRamReadOutOfBounds,
        DataRamWriteOutOfBounds,
        InvalidOutput,
        UnsupportedEmit,
        OutsideCode,
        StepLimit,
    };

    static std::string_view ErrorText(Word error) {
        constexpr std::array<std::string_view, 13> texts{
            "",
            "parameter FIFO underflow",
            "invalid source register",
            "invalid destination register",
            "unsupported extended operation",
            "invalid merge destination",
            "invalid loop",
            "data RAM read out of bounds",
            "data RAM write out of bounds",
            "invalid output selector",
            "unsupported emitted method",
            "instruction outside uploaded RAM",
            "instruction limit exceeded",
        };
        return error < texts.size() ? texts[error] : "unknown";
    }

    /// Calls of each entry point that also run the interpreter after a compile.
    static constexpr std::uint8_t CheckedCalls = 64;

    /// Compiles `code` (3 words per instruction) when it differs from what was compiled.
    void Prepare(std::span<const Word> code, std::uint64_t generation) {
        if (generation == compiled_generation) {
            return;
        }
        compiled_generation = generation;
        program.reset();
        diverged = false;
        try {
            program = std::make_unique<Program>(code);
        } catch (const std::exception& exception) {
            if (!compile_failure_logged.exchange(true)) {
                LOG_ERROR(HW_GPU, "Turing macro JIT: could not compile the macro code to ARM64 ({}); "
                                  "it runs in the interpreter", exception.what());
            }
            return;
        }
        compiled_code.assign(code.begin(), code.end());
        checks_left.assign(program->instruction_count + 1, CheckedCalls);
        if (!active_logged.exchange(true)) {
            LOG_INFO(HW_GPU, "Turing macro JIT: macro code compiled to ARM64 ({} instructions, "
                             "{} KiB); the first {} calls of each entry are checked against the "
                             "interpreter",
                     program->instruction_count, program->code_bytes / 1024, CheckedCalls);
        }
    }

    /// False when the code could not be compiled or differed from the interpreter: the caller
    /// then runs the macro memo and the interpreter.
    bool Usable() const {
        return program != nullptr && !diverged;
    }

    template <typename Read, typename Emit>
    Result Execute(Word start, std::vector<Word>& parameters, std::span<Word> data_ram, Read read,
                   Emit emit) {
        if (!program) [[unlikely]] {
            return {false, 0, 0, ErrorText(OutsideCode)};
        }
        std::uint8_t& left = checks_left[(std::min<std::size_t>)(start, checks_left.size() - 1)];
        if (left != 0) [[unlikely]] {
            --left;
            return ExecuteChecked(start, parameters, data_ram, read, emit);
        }
        return Run(start, parameters, data_ram, read, emit);
    }

    /// The code for `code` without its jump table (see Relocate), for tests.
    struct Assembly {
        std::vector<std::uint32_t> words;
        std::size_t table_offset{};
        std::size_t entry_offset{};
        /// Byte offset of each instruction's code.
        std::vector<std::size_t> targets;
    };

    static Assembly Assemble(std::span<const Word> code) {
        return Assembler{code}.Finish();
    }

    /// Writes the jump table of `assembly`, copied to `words`, for code placed at `base`.
    static void Relocate(std::span<std::uint32_t> words, const Assembly& assembly,
                         std::uintptr_t base) {
        for (std::size_t pc = 0; pc < assembly.targets.size(); ++pc) {
            const std::uint64_t address = std::uint64_t{base} + assembly.targets[pc];
            const std::size_t index = assembly.table_offset / sizeof(std::uint32_t) + pc * 2;
            words[index] = static_cast<std::uint32_t>(address);
            words[index + 1] = static_cast<std::uint32_t>(address >> 32);
        }
    }

private:
    using Decoded = TuringMacroInterpreter::Decoded;

    template <typename Read, typename Emit>
    struct Thunks {
        Read& read;
        Emit& emit;
        static bool CallEmit(State* state, Word method, Word value) {
            const bool ok = static_cast<Thunks*>(state->user)->emit(method, value);
            // MME_DMA_READ_FIFOED appends to the FIFO from inside the emit.
            state->params = state->fifo->data();
            state->params_size = static_cast<Word>(state->fifo->size());
            return ok;
        }
        static Word CallRead(State* state, Word address) {
            return static_cast<Thunks*>(state->user)->read(address);
        }
    };

    template <typename Read, typename Emit>
    Result Run(Word start, std::vector<Word>& parameters, std::span<Word> data_ram, Read& read,
               Emit& emit) {
        State state;
        state.regs.fill(0);
        state.carry = state.method = state.increment = 0;
        state.loop_count = state.loop_start = state.loop_end = 0;
        state.consumed = 0;
        state.fail_pc = 0;
        state.end_next = state.stopped = state.cancel_end = 0;
        state.start = start;
        state.steps = 1000000;
        state.params = parameters.data();
        state.params_size = static_cast<Word>(parameters.size());
        state.dram = data_ram.data();
        state.fifo = &parameters;
        Thunks<Read, Emit> thunks{read, emit};
        state.user = &thunks;
        state.emit = &Thunks<Read, Emit>::CallEmit;
        state.read = &Thunks<Read, Emit>::CallRead;
        const Word error = program->entry(&state);
        if (error == None) {
            return {true, state.fail_pc, state.consumed, {}};
        }
        return {false, state.fail_pc, state.consumed, ErrorText(error)};
    }

    struct AssemblerBuffer {
        std::vector<std::uint32_t> words;
    };

    // Register use: x19 State*; w20/w21 the instruction's FIFO loads; w22/w23 its ALU results;
    // x24 the jump table. w0 (value), w1 (x) and w2 (y) as eax/ecx/edx in the x86-64 code;
    // x16/x17 scratch. Callee-saved x19-x24, x29 and x30 are kept on a 64-byte frame, so sp stays
    // 16-byte aligned at the callbacks.
    class Assembler : private AssemblerBuffer, public oaknut::VectorCodeGenerator {
    public:
        explicit Assembler(std::span<const Word> code) : oaknut::VectorCodeGenerator(words) {
            Generate(code);
        }

        Assembly Finish() {
            Assembly assembly;
            assembly.table_offset = table_offset;
            assembly.entry_offset = entry_offset;
            assembly.targets.reserve(labels.size());
            for (const oaknut::Label& label : labels) {
                assembly.targets.push_back(static_cast<std::size_t>(label.offset()));
            }
            assembly.words = std::move(words);
            return assembly;
        }

    private:
        using Cond = oaknut::Cond;
        using WReg = oaknut::WReg;
        using XReg = oaknut::XReg;
        using Label = oaknut::Label;

        static constexpr WReg W0{0}, W1{1}, W2{2}, W16{16}, W17{17};
        static constexpr XReg X0{0}, X1{1}, X16{16}, X19{19}, X20{20}, X21{21}, X22{22}, X23{23},
            X24{24}, X29{29}, X30{30};
        static constexpr oaknut::WzrReg WZR{};
        static constexpr oaknut::SpReg sp{};
        static constexpr XReg st{19};
        static constexpr WReg load0{20}, load1{21}, alu0{22}, alu1{23};
        static constexpr XReg table{24};

        static constexpr std::size_t RegOffset(Word reg) {
            return offsetof(State, regs) + reg * sizeof(Word);
        }

        static WReg Alu(Word lane) {
            return lane == 0 ? alu0 : alu1;
        }

        void Generate(std::span<const Word> code) {
            const Word count = static_cast<Word>(code.size() / 3);
            labels.resize(count);
            std::vector<Decoded> decoded(count);
            for (Word pc = 0; pc < count; ++pc) {
                decoded[pc] = TuringMacroInterpreter::Decode(
                    {code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
            }
            Analyze(decoded);

            // The jump table comes first (Relocate fills it), within ADR range of the entry.
            table_offset = static_cast<std::size_t>(offset());
            l(table_label);
            for (Word pc = 0; pc < count; ++pc) {
                dx(0);
            }

            entry_offset = static_cast<std::size_t>(offset());
            STP(X29, X30, sp, oaknut::PreIndexed{}, -64);
            MOV(X29, sp);
            STP(X19, X20, sp, 16);
            STP(X21, X22, sp, 32);
            STP(X23, X24, sp, 48);
            MOV(st, X0);
            ADR(table, table_label);
            LDR(W0, st, offsetof(State, start));
            CmpImm(W0, count);
            B(Cond::HS, Far(fail_outside));
            LDR(X16, table, X0, oaknut::IndexExt::LSL, 3);
            BR(X16);

            for (Word pc = 0; pc < count; ++pc) {
                // B.cond and CBZ reach 1 MiB: out-of-line jumps to the shared labels go through
                // stubs placed every 256 KiB.
                if (!stubs.empty() && static_cast<std::size_t>(offset()) - window_start > 256 * 1024) {
                    FlushStubs(true);
                }
                l(labels[pc]);
                Instruction(pc, count, decoded[pc]);
            }
            // Every instruction ends with a jump.
            FlushStubs(false);

            // Dispatch on w0 (the next pc). Every backward edge comes through here, so the
            // instruction limit is counted here.
            l(dispatch);
            LDR(W16, st, offsetof(State, steps));
            SUBS(W16, W16, 1);
            STR(W16, st, offsetof(State, steps));
            B(Cond::EQ, fail_steps);
            CmpImm(W0, count);
            B(Cond::HS, fail_outside);
            LDR(X16, table, X0, oaknut::IndexExt::LSL, 3);
            BR(X16);

            l(success);
            MOV(W0, WZR);
            l(leave);
            LDP(X23, X24, sp, 48);
            LDP(X21, X22, sp, 32);
            LDP(X19, X20, sp, 16);
            LDP(X29, X30, sp, oaknut::PostIndexed{}, 64);
            RET();
            const auto fail_with = [&](Label& label, Word error) {
                l(label);
                MOV(W0, error);
                B(leave);
            };
            fail_with(fail_underflow, FifoUnderflow);
            fail_with(fail_source, InvalidSource);
            fail_with(fail_destination, InvalidDestination);
            fail_with(fail_extended, UnsupportedExtended);
            fail_with(fail_merge, InvalidMerge);
            fail_with(fail_loop, InvalidLoop);
            fail_with(fail_dread, DataRamReadOutOfBounds);
            fail_with(fail_dwrite, DataRamWriteOutOfBounds);
            fail_with(fail_output, InvalidOutput);
            fail_with(fail_emit, UnsupportedEmit);
            fail_with(fail_outside, OutsideCode);
            fail_with(fail_steps, StepLimit);
        }

        // A label near the current position that jumps to `target` (placed by FlushStubs).
        Label& Far(Label& target) {
            for (const auto& stub : stubs) {
                if (stub->target == &target) {
                    return stub->label;
                }
            }
            if (stubs.empty()) {
                window_start = static_cast<std::size_t>(offset());
            }
            stubs.push_back(std::make_unique<Stub>());
            stubs.back()->target = &target;
            return stubs.back()->label;
        }

        void FlushStubs(bool reachable) {
            if (stubs.empty()) {
                return;
            }
            Label over;
            if (reachable) {
                B(over);
            }
            for (const auto& stub : stubs) {
                l(stub->label);
                B(*stub->target);
            }
            if (reachable) {
                l(over);
            }
            stubs.clear();
        }

        void CmpImm(WReg reg, Word value) {
            if (value < 4096) {
                CMP(reg, value);
            } else {
                MOV(W17, value);
                CMP(reg, W17);
            }
        }

        // Loads MME register `reg` (as an operand of `lane`) into `dst`.
        void Get(WReg dst, const Decoded& d, Word reg, Word lane) {
            if (reg < 24) {
                LDR(dst, st, RegOffset(reg));
                return;
            }
            switch (reg) {
            case 24: MOV(dst, WZR); return;
            case 25: MOV(dst, Word(std::int32_t(std::int16_t(d.imm[lane])))); return;
            case 26: MOV(dst, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))); return;
            case 27: MOV(dst, (d.imm[0] << 16) | d.imm[1]); return;
            case 28: MOV(dst, load0); return;
            case 29: MOV(dst, load1); return;
            default: B(fail_source); return;
            }
        }

        static bool IsFlow(Word op) {
            return op >= 23 && op <= 29;
        }

        // Where a loop can end (its back edge is checked only there) and which instructions can
        // run right after an END (only those check for the end of the macro).
        void Analyze(const std::vector<Decoded>& decoded) {
            const Word count = static_cast<Word>(decoded.size());
            loop_end.assign(count, false);
            after_end.assign(count, false);
            const auto mark = [&](std::vector<bool>& set, std::int64_t pc) {
                if (pc >= 0 && pc < count) set[static_cast<std::size_t>(pc)] = true;
            };
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2 && pc + d.imm[lane] - 1 < count) {
                        loop_end[pc + d.imm[lane] - 1] = true;
                    }
                }
            }
            for (Word pc = 0; pc < count; ++pc) {
                const Decoded& d = decoded[pc];
                if (!d.end) continue;
                mark(after_end, std::int64_t{pc} + 1);
                for (Word lane = 0; lane < 2; ++lane) {
                    const Word imm = d.imm[lane];
                    switch (d.op[lane]) {
                    case 23: mark(after_end, std::int64_t{pc} + imm); break;
                    case 24: mark(after_end, std::int64_t{pc} + (imm & 0x7fff)); break;
                    case 25: case 26: case 27: case 28: case 29:
                        mark(after_end, std::int64_t{pc} + (std::int32_t(imm << 19) >> 19));
                        break;
                    default: break;
                    }
                }
            }
            // An END at a loop's last instruction can be followed by the loop's second one.
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2) {
                        const Word end = pc + d.imm[lane] - 1;
                        if (end < count && decoded[end].end) mark(after_end, std::int64_t{pc} + 1);
                    }
                }
            }
        }

        void Instruction(Word pc, Word count, const Decoded& d) {
            // FIFO loads, in lane order.
            for (Word i = 0; i < 2; ++i) {
                const WReg load = i == 0 ? load0 : load1;
                if (!d.load[i]) {
                    MOV(load, WZR);
                    continue;
                }
                LDR(W0, st, offsetof(State, consumed));
                LDR(W16, st, offsetof(State, params_size));
                CMP(W0, W16);
                B(Cond::HS, Far(fail_underflow));
                LDR(X16, st, offsetof(State, params));
                LDR(load, X16, X0, oaknut::IndexExt::LSL, 2);
                ADD(W0, W0, 1);
                STR(W0, st, offsetof(State, consumed));
            }
            if (!d.pred_mode) {
                Body(pc, count, d, 15, true);
                return;
            }
            Label when_false;
            Get(W0, d, d.pred_reg, 0);
            CBZ(W0, when_false);
            Body(pc, count, d, d.mask_true, false);
            l(when_false);
            Body(pc, count, d, d.mask_false, true);
        }

        // `last`: the code that follows is the next instruction's.
        void Body(Word pc, Word count, const Decoded& d, Word mask, bool last) {
            // An inactive lane's result reads as 0 when an output selects it.
            for (Word lane = 0; lane < 2; ++lane) {
                if (mask & (1U << lane)) continue;
                bool selected = false;
                for (Word out = 0; out < 2; ++out) {
                    if (!(mask & (4U << out))) continue;
                    selected |= d.out_method[out] == lane + 1 || d.out_emit[out] == lane + 1;
                }
                if (selected) {
                    MOV(Alu(lane), WZR);
                }
            }
            const bool flow = ((mask & 1) && IsFlow(d.op[0])) || ((mask & 2) && IsFlow(d.op[1]));
            if (flow) {
                MOV(W16, pc + 1);
                STR(W16, st, offsetof(State, next_pc));
                STRB(WZR, st, offsetof(State, cancel_end));
            }
            // Lane 1's operands are read before lane 0 commits its destination (or calls out).
            const bool lane0_active = (mask & 1) != 0;
            const bool early_operands =
                (mask & 2) && lane0_active &&
                (d.op[0] == 22 || (d.dst[0] < 24 && (d.dst[0] == d.src_a[1] || d.dst[0] == d.src_b[1])));
            if (early_operands) {
                Get(W0, d, d.src_a[1], 1);
                STR(W0, st, offsetof(State, scratch_x));
                Get(W0, d, d.src_b[1], 1);
                STR(W0, st, offsetof(State, scratch_y));
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (1U << lane))) {
                    continue;
                }
                if (lane == 1 && early_operands) {
                    LDR(W1, st, offsetof(State, scratch_x));
                    LDR(W2, st, offsetof(State, scratch_y));
                } else {
                    Get(W1, d, d.src_a[lane], lane);
                    Get(W2, d, d.src_b[lane], lane);
                }
                Lane(pc, d, lane);
                // w0 holds the lane's value.
                MOV(Alu(lane), W0);
                if (d.dst[lane] < 24) {
                    STR(W0, st, RegOffset(d.dst[lane]));
                } else if (d.dst[lane] != 24) {
                    B(fail_destination);
                }
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (4U << lane))) {
                    continue;
                }
                if (d.out_method[lane]) {
                    Output(W0, d, d.out_method[lane]);
                    AND(W1, W0, 0xfff);
                    STR(W1, st, offsetof(State, method));
                    UBFX(W0, W0, 12, 4);
                    STR(W0, st, offsetof(State, increment));
                }
                if (d.out_emit[lane]) {
                    Output(W2, d, d.out_emit[lane]);
                    LDR(W1, st, offsetof(State, method));
                    MOV(X0, st);
                    LDR(X16, st, offsetof(State, emit));
                    BLR(X16);
                    TST(W0, 0xff);
                    B(Cond::EQ, Far(fail_emit));
                    LDR(W0, st, offsetof(State, increment));
                    LDR(W1, st, offsetof(State, method));
                    ADD(W1, W1, W0);
                    STR(W1, st, offsetof(State, method));
                }
            }
            // An END in the previous instruction ends the macro after this one.
            if (after_end[pc]) {
                LDRB(W16, st, offsetof(State, end_next));
                CBNZ(W16, Far(success));
            }
            if (flow) {
                LDRB(W16, st, offsetof(State, stopped));
                CBNZ(W16, Far(success));
            }
            if (d.end) {
                if (flow) {
                    LDRB(W16, st, offsetof(State, cancel_end));
                    EOR(W16, W16, 1);
                    STRB(W16, st, offsetof(State, end_next));
                } else {
                    MOV(W16, 1);
                    STRB(W16, st, offsetof(State, end_next));
                }
            }
            if (loop_end[pc]) {
                Label no_loop;
                LDR(W0, st, offsetof(State, loop_count));
                CBZ(W0, no_loop);
                LDR(W16, st, offsetof(State, loop_end));
                CmpImm(W16, pc);
                B(Cond::NE, no_loop);
                SUB(W0, W0, 1);
                STR(W0, st, offsetof(State, loop_count));
                LDR(W0, st, offsetof(State, loop_start));
                ADD(W0, W0, 1);
                B(dispatch);
                l(no_loop);
            }
            if (flow) {
                LDR(W0, st, offsetof(State, next_pc));
                if (pc + 1 < count) {
                    CmpImm(W0, pc + 1);
                    B(Cond::NE, Far(dispatch));
                    if (!last) {
                        B(labels[pc + 1]);
                    }
                } else {
                    B(dispatch);
                }
            } else if (pc + 1 < count) {
                if (!last) {
                    B(labels[pc + 1]);
                }
            } else {
                B(fail_outside);
            }
        }

        void Output(WReg dst, const Decoded& d, Word selector) {
            switch (selector) {
            case 1: MOV(dst, alu0); return;
            case 2: MOV(dst, alu1); return;
            case 3: MOV(dst, load0); return;
            case 4: MOV(dst, load1); return;
            case 5: MOV(dst, d.imm[0]); return;
            case 6: MOV(dst, d.imm[1]); return;
            case 8: MOV(dst, d.imm[0] >> 12); return;
            case 9: MOV(dst, d.imm[1] >> 12); return;
            case 10: MOV(dst, (d.imm[0] << 16) | d.imm[1]); return;
            default: B(fail_output); return;
            }
        }

        // The condition of comparison `op` (CMP x, y).
        static Cond Condition(Word op) {
            switch (op) {
            case 17: case 25: return Cond::LT;
            case 18: case 26: return Cond::LO;
            case 19: case 27: return Cond::LE;
            case 20: case 28: return Cond::LS;
            default: return Cond::EQ;
            }
        }

        // w1 = x, w2 = y; leaves the value in w0.
        void Lane(Word pc, const Decoded& d, Word lane) {
            const Word imm = d.imm[lane];
            const Word op = d.op[lane];
            switch (op) {
            case 0:
                ADDS(W0, W1, W2);
                CSET(W16, Cond::CS);
                STR(W16, st, offsetof(State, carry));
                return;
            case 1:
                ADD(W0, W1, W2);
                LDR(W16, st, offsetof(State, carry));
                ADD(W0, W0, W16);
                return;
            case 2:
                // The carry is the borrow (x < y): ARM64's C flag is its inverse.
                SUBS(W0, W1, W2);
                CSET(W16, Cond::LO);
                STR(W16, st, offsetof(State, carry));
                return;
            case 3:
                SUB(W0, W1, W2);
                LDR(W16, st, offsetof(State, carry));
                SUB(W0, W0, W16);
                return;
            case 4:
                SMULL(X0, W1, W2);
                LSR(X16, X0, 32);
                STR(W16, st, offsetof(State, carry));
                MOV(W0, W0);
                return;
            case 6:
                UMULL(X0, W1, W2);
                LSR(X16, X0, 32);
                STR(W16, st, offsetof(State, carry));
                MOV(W0, W0);
                return;
            case 5:
                LDR(W0, st, offsetof(State, carry));
                return;
            case 7:
                CmpImm(W1, 0x1000);
                B(Cond::NE, Far(fail_extended));
                MOV(W0, WZR);
                return;
            case 8:
                CLZ(W0, W1);
                return;
            case 9: LSL(W0, W1, W2); return;
            case 10: LSR(W0, W1, W2); return;
            case 11: ASR(W0, W1, W2); return;
            case 12: AND(W0, W1, W2); return;
            case 13: AND(W0, W1, W2); MVN(W0, W0); return;
            case 14: ORR(W0, W1, W2); return;
            case 15: EOR(W0, W1, W2); return;
            case 16: {
                const Word dest = (imm >> 10) & 63, bit_count = (imm >> 5) & 31;
                const Word source = imm & 31;
                if (dest >= 32) {
                    B(fail_merge);
                    return;
                }
                const Word bits_mask = (1U << bit_count) - 1;
                MOV(W16, ~(bits_mask << dest));
                AND(W0, W1, W16);
                LSR(W2, W2, source);
                MOV(W16, bits_mask);
                AND(W2, W2, W16);
                LSL(W2, W2, dest);
                ORR(W0, W0, W2);
                return;
            }
            case 17: case 18: case 19: case 20: case 21:
                CMP(W1, W2);
                CSETM(W0, Condition(op));
                return;
            case 22:
                ADD(W16, W1, W2);
                AND(W1, W16, 0xffff);
                MOV(X0, st);
                LDR(X16, st, offsetof(State, read));
                BLR(X16);
                return;
            case 23: {
                if (imm < 2) {
                    B(fail_loop);
                    return;
                }
                Label skip, done;
                LDR(W16, st, offsetof(State, loop_count));
                CBNZ(W16, Far(fail_loop));
                CBZ(W1, skip);
                SUB(W1, W1, 1);
                STR(W1, st, offsetof(State, loop_count));
                MOV(W16, pc);
                STR(W16, st, offsetof(State, loop_start));
                MOV(W16, pc + imm - 1);
                STR(W16, st, offsetof(State, loop_end));
                B(done);
                l(skip);
                MOV(W16, pc + imm);
                STR(W16, st, offsetof(State, next_pc));
                l(done);
                MOV(W0, WZR);
                return;
            }
            case 24:
                MOV(W16, pc + (imm & 0x7fff));
                STR(W16, st, offsetof(State, next_pc));
                MOV(W0, WZR);
                return;
            case 25: case 26: case 27: case 28: case 29: {
                // Taken when the comparison equals bit 15.
                Label not_taken;
                CMP(W1, W2);
                const Cond condition = Condition(op);
                B((imm & 0x8000) ? oaknut::invert(condition) : condition, not_taken);
                const auto branch_offset = std::int32_t(imm << 19) >> 19;
                if (branch_offset == -4096) {
                    MOV(W16, 1);
                    STRB(W16, st, offsetof(State, stopped));
                } else {
                    MOV(W16, Word(std::int32_t(pc) + branch_offset));
                    STR(W16, st, offsetof(State, next_pc));
                    if (imm & 0x4000) {
                        MOV(W16, 1);
                        STRB(W16, st, offsetof(State, cancel_end));
                    } else {
                        STRB(WZR, st, offsetof(State, cancel_end));
                    }
                }
                l(not_taken);
                MOV(W0, WZR);
                return;
            }
            case 30:
                CmpImm(W1, 0xc00);
                B(Cond::HS, Far(fail_dread));
                LDR(X16, st, offsetof(State, dram));
                LDR(W0, X16, X1, oaknut::IndexExt::LSL, 2);
                return;
            case 31:
                CmpImm(W1, 0xc00);
                B(Cond::HS, Far(fail_dwrite));
                LDR(X16, st, offsetof(State, dram));
                STR(W2, X16, X1, oaknut::IndexExt::LSL, 2);
                MOV(W0, WZR);
                return;
            default:
                MOV(W0, WZR);
                return;
            }
        }

        struct Stub {
            Label label;
            Label* target{};
        };

        std::vector<Label> labels;
        std::vector<bool> loop_end, after_end;
        std::vector<std::unique_ptr<Stub>> stubs;
        std::size_t window_start{};
        std::size_t table_offset{}, entry_offset{};
        Label table_label, dispatch, success, leave;
        Label fail_underflow, fail_source, fail_destination, fail_extended, fail_merge, fail_loop,
            fail_dread, fail_dwrite, fail_output, fail_emit, fail_outside, fail_steps;
    };

    // Pages for code (macro_turing_jit_arm64.cpp): writable when allocated, read-only and
    // executable (with the instruction cache invalidated) once sealed.
    static void* AllocateCode(std::size_t bytes);
    static bool SealCode(void* memory, std::size_t bytes);
    static void FreeCode(void* memory, std::size_t bytes);

    // The assembled code in its own pages.
    struct Program {
        explicit Program(std::span<const Word> code) {
            Assembly assembly = Assemble(code);
            instruction_count = static_cast<Word>(assembly.targets.size());
            code_bytes = assembly.words.size() * sizeof(std::uint32_t);
            memory = AllocateCode(code_bytes);
            if (memory == nullptr) {
                throw std::bad_alloc{};
            }
            const auto base = reinterpret_cast<std::uintptr_t>(memory);
            std::span<std::uint32_t> words{static_cast<std::uint32_t*>(memory), assembly.words.size()};
            std::copy(assembly.words.begin(), assembly.words.end(), words.begin());
            Relocate(words, assembly, base);
            if (!SealCode(memory, code_bytes)) {
                FreeCode(memory, code_bytes);
                memory = nullptr;
                throw std::bad_alloc{};
            }
            entry = reinterpret_cast<Word (*)(State*)>(base + assembly.entry_offset);
        }

        ~Program() {
            if (memory != nullptr) {
                FreeCode(memory, code_bytes);
            }
        }

        Program(const Program&) = delete;
        Program& operator=(const Program&) = delete;

        Word (*entry)(State*) = nullptr;
        Word instruction_count{};
        std::size_t code_bytes{};
        void* memory{};
    };

    // What a checked call saw and did, for the interpreter's replay.
    struct Check {
        struct ReadRecord {
            Word address, value;
        };
        struct EmitRecord {
            Word method, value;
            bool ok;
            // Ends of this emit's FIFO words in `words` and of its data RAM changes in `changes`.
            std::size_t words_end, changes_end;
        };
        std::vector<Word> parameters;
        TuringMacroInterpreter::DataRam dram_before;
        TuringMacroInterpreter::DataRam snapshot;
        std::vector<ReadRecord> reads;
        std::vector<EmitRecord> emits;
        std::vector<Word> words;
        std::vector<std::pair<Word, Word>> changes;
        bool comparable;
        TuringMacroInterpreter reference;
        std::vector<Word> fifo;
    };

    template <typename Read, typename Emit>
    Result ExecuteChecked(Word start, std::vector<Word>& parameters, std::span<Word> data_ram,
                          Read& read, Emit& emit) {
        if (data_ram.size() != std::tuple_size_v<TuringMacroInterpreter::DataRam>) {
            return Run(start, parameters, data_ram, read, emit);
        }
        if (!check) {
            check = std::make_unique<Check>();
        }
        Check& c = *check;
        c.parameters.assign(parameters.begin(), parameters.end());
        std::copy(data_ram.begin(), data_ram.end(), c.dram_before.begin());
        c.reads.clear();
        c.emits.clear();
        c.words.clear();
        c.changes.clear();
        c.comparable = true;
        auto recording_read = [&](Word address) -> Word {
            const Word value = read(address);
            c.reads.push_back({address, value});
            return value;
        };
        auto recording_emit = [&](Word method, Word value) -> bool {
            std::copy(data_ram.begin(), data_ram.end(), c.snapshot.begin());
            const std::size_t fifo_size = parameters.size();
            const bool ok = emit(method, value);
            if (parameters.size() >= fifo_size) {
                c.words.insert(c.words.end(),
                               parameters.begin() + static_cast<std::ptrdiff_t>(fifo_size),
                               parameters.end());
            } else {
                c.comparable = false;
            }
            if (!std::equal(data_ram.begin(), data_ram.end(), c.snapshot.begin())) {
                for (std::size_t i = 0; i < data_ram.size(); ++i) {
                    if (data_ram[i] != c.snapshot[i]) {
                        c.changes.emplace_back(static_cast<Word>(i), data_ram[i]);
                    }
                }
            }
            c.emits.push_back({method, value, ok, c.words.size(), c.changes.size()});
            return ok;
        };
        const Result result = Run(start, parameters, data_ram, recording_read, recording_emit);
        if (c.comparable) {
            Compare(start, result, data_ram);
        }
        return result;
    }

    static void AppendWords(std::string& text, std::span<const Word> values, std::size_t limit) {
        for (std::size_t i = 0; i < (std::min)(values.size(), limit); ++i) {
            fmt::format_to(std::back_inserter(text), "{}{:08x}", i == 0 ? "" : ",", values[i]);
        }
        if (values.size() > limit) {
            text += ",...";
        }
    }

    // Runs the interpreter on the checked call's inputs, replaying what the compiled code saw,
    // and compares everything both did.
    void Compare(Word start, const Result& result, std::span<const Word> data_ram) {
        Check& c = *check;
        TuringMacroInterpreter& reference = c.reference;
        reference.dram = c.dram_before;
        reference.active_dram = &reference.dram;
        c.fifo.assign(c.parameters.begin(), c.parameters.end());
        std::size_t reads_done = 0, emits_done = 0;
        std::string difference;
        const auto differ = [&](std::string text) {
            if (difference.empty()) difference = std::move(text);
        };
        const auto replay_read = [&](Word address) -> Word {
            const std::size_t index = reads_done++;
            if (index >= c.reads.size()) {
                differ(fmt::format("read {} of {:#x}: the compiled code made only {} reads", index,
                                   address, c.reads.size()));
                return 0;
            }
            if (c.reads[index].address != address) {
                differ(fmt::format("read {}: interpreter {:#x}, compiled {:#x}", index, address,
                                   c.reads[index].address));
            }
            return c.reads[index].value;
        };
        const auto replay_emit = [&](Word method, Word value) -> bool {
            const std::size_t index = emits_done++;
            if (index >= c.emits.size()) {
                differ(fmt::format("emit {} ({:#x} = {:#x}): the compiled code made only {} emits",
                                   index, method, value, c.emits.size()));
                return false;
            }
            const auto& record = c.emits[index];
            if (record.method != method || record.value != value) {
                differ(fmt::format("emit {}: interpreter {:#x} = {:#x}, compiled {:#x} = {:#x}", index,
                                   method, value, record.method, record.value));
                return false;
            }
            const std::size_t words_begin = index == 0 ? 0 : c.emits[index - 1].words_end;
            const std::size_t changes_begin = index == 0 ? 0 : c.emits[index - 1].changes_end;
            c.fifo.insert(c.fifo.end(), c.words.begin() + static_cast<std::ptrdiff_t>(words_begin),
                          c.words.begin() + static_cast<std::ptrdiff_t>(record.words_end));
            for (std::size_t i = changes_begin; i < record.changes_end; ++i) {
                reference.dram[c.changes[i].first] = c.changes[i].second;
            }
            return record.ok;
        };
        const Result expected =
            reference.Execute(compiled_code, start, c.fifo, replay_read, replay_emit);

        const std::string_view error = result.success ? std::string_view{} : result.error;
        // Calls the x86-64 and ARM64 code run differently from the interpreter on purpose: they
        // read never-written registers as 0, count the instruction limit on jumps only, and run
        // lane 0 before reading lane 1's operands (a failing instruction can report another
        // error). Those are not compared.
        const bool uncomparable =
            difference.empty() && !expected.success &&
            (expected.error == "uninitialized register read" ||
             expected.error == "instruction limit exceeded" ||
             (!result.success && expected.error != error && expected.consumed == result.consumed &&
              emits_done == c.emits.size()));
        if (uncomparable) {
            if (!uncomparable_logged.exchange(true)) {
                LOG_WARNING(HW_GPU, "Turing macro JIT: a call at start={:#x} was not compared "
                                    "(interpreter: {}, compiled: {})",
                            start, expected.error, result.success ? std::string_view{"success"} : error);
            }
            return;
        }
        if (difference.empty()) {
            if (expected.success != result.success || expected.error != error) {
                differ(fmt::format("result: interpreter {} '{}', compiled {} '{}'",
                                   expected.success ? "success" : "failure", expected.error,
                                   result.success ? "success" : "failure", error));
            } else if (expected.consumed != result.consumed) {
                differ(fmt::format("FIFO words consumed: interpreter {}, compiled {}",
                                   expected.consumed, result.consumed));
            } else if (emits_done != c.emits.size()) {
                differ(fmt::format("emits: interpreter {}, compiled {}", emits_done, c.emits.size()));
            } else if (result.success && reads_done != c.reads.size()) {
                // (A failing instruction's lanes can differ in how far they got.)
                differ(fmt::format("reads: interpreter {}, compiled {}", reads_done, c.reads.size()));
            } else if (result.success) {
                for (std::size_t i = 0; i < data_ram.size(); ++i) {
                    if (reference.dram[i] != data_ram[i]) {
                        differ(fmt::format("data RAM[{:#x}]: interpreter {:#x}, compiled {:#x}", i,
                                           reference.dram[i], data_ram[i]));
                        break;
                    }
                }
            }
        }
        if (difference.empty()) {
            if (matched_calls.fetch_add(1) + 1 == 1000) {
                LOG_INFO(HW_GPU, "Turing macro JIT: the first 1000 checked calls matched the "
                                 "interpreter");
            }
            return;
        }
        diverged = true;
        if (!difference_logged.exchange(true)) {
            std::string text = fmt::format(
                "Turing macro JIT: the ARM64 code differs from the interpreter, start={:#x}: {}. "
                "Compiled: {} '{}', consumed {}, {} emits, {} reads; interpreter: {} '{}', pc {:#x}, "
                "consumed {}. Parameters ",
                start, difference, result.success ? "success" : "failure", error, result.consumed,
                c.emits.size(), c.reads.size(), expected.success ? "success" : "failure",
                expected.error, expected.pc, expected.consumed);
            AppendWords(text, c.parameters, 32);
            text += "; code from start ";
            const std::size_t begin = (std::min<std::size_t>)(std::size_t{start} * 3, compiled_code.size());
            AppendWords(text,
                        std::span<const Word>{compiled_code}.subspan(
                            begin, (std::min<std::size_t>)(48, compiled_code.size() - begin)),
                        48);
            text += ". This macro code now runs in the interpreter.";
            LOG_ERROR(HW_GPU, "{}", text);
        }
    }

    std::unique_ptr<Program> program;
    std::uint64_t compiled_generation{~std::uint64_t{0}};
    bool diverged{};
    // The compiled code (the interpreter's input when checking) and the checks left per entry
    // (the last one counts entries outside the code).
    std::vector<Word> compiled_code;
    std::vector<std::uint8_t> checks_left;
    std::unique_ptr<Check> check;

    static inline std::atomic<bool> active_logged{};
    static inline std::atomic<bool> compile_failure_logged{};
    static inline std::atomic<bool> difference_logged{};
    static inline std::atomic<bool> uncomparable_logged{};
    static inline std::atomic<std::uint64_t> matched_calls{};
};

} // namespace Tegra
