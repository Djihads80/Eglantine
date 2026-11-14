// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/macro_turing.h"

// List-fed grid macros (EGLA-146): compute macros whose three parameters a command list on a
// GPU-written page holds, and which only store them in the macro data RAM as a later dispatch's
// grid. Mario Kart World and Ocarina of Time call such a macro (CodeHash 055743923e135e09) many
// times a frame. To parse those three words the split's front waited for the back to run the work
// that writes them, for the host GPU to run it and for a copy back (DmaPusher::
// ReadGpuWrittenCommands): one round trip per call.
//
// The front now takes such a call without reading its words (DmaPusher::TakeListGrid):
// - At the list's place in the stream the back copies the three words into a kept-words slot
//   (RasterizerInterface::Nx2KeepListWords).
// - The front stores marker words in the data RAM instead (Marker: each names the list's keep id
//   and which of the three words it stands for).
// - While a marker is in the data RAM or in a compute register, every macro that may read it
//   first runs dry with the markers followed through it (DryRun). It runs on the markers only if
//   it moves them unchanged into inline upload data (0x6d) or into the grid register of the QMD
//   word they stand for (0xd4 + word). Anything else, and any 3D macro that reads them, gets the
//   real words first (ResolveWord: the kept copy, read back at the old cost once). Only a list
//   that is neither kept nor mapped leaves zeros, never a marker.
// - A list's kept slot is reused only after its live markers got their real words (ClaimSlot):
//   data RAM words and QMD grid registers are resolved then, and a slot whose list an upload still
//   on its way carries is not reused (the next list is read the old way).
// - On the back, an inline write that carries markers gets the kept words copied over them on the
//   host GPU (RasterizerVulkan::AccelerateInlineToMemory), so every reader of those bytes sees the
//   real words, and a QMD whose grid is a list's three markers dispatches indirectly from the kept
//   copy (QmdGridSource). A QMD marker is trusted only at an address the back patched from that
//   same list, with no write there since, while the list is kept (QmdWordList); any other QMD
//   marker takes the patched words from the host GPU (KeplerCompute's QMD read).
//
// EDEN_NX2_LIST_HLE_VERIFY=1 also reads each list's words the old way and compares them with the
// kept copy, every guarded macro's run on the markers with a run on the real words, and the words
// each patched upload and indirect dispatch takes (NoteVerify).
//
// Ported from #1336 (StoreGridCells, the dry run that follows the grid words, the kept-words
// slots) and jpkto's #805 (the device limits an indirect dispatch needs, 16-bit height and depth).
namespace Tegra::Nx2ListGrid {

using Word = TuringMacroInterpreter::Word;
using DataRam = TuringMacroInterpreter::DataRam;
/// The data RAM words a macro stores a grid in: x, y, z.
using GridCells = std::array<u32, 3>;

inline constexpr u32 ExecUploadMethod = 0x6c;
inline constexpr u32 DataUploadMethod = 0x6d;
/// The compute engine's copy of the inline QMD starts at 0xc8: words 12 to 14 are its grid.
inline constexpr u32 QmdGridRegister = 0xc8 + 12;

/// EDEN_NX2_LIST_HLE_VERIFY=1 (verify mode, off by default).
inline bool g_verify = std::getenv("EDEN_NX2_LIST_HLE_VERIFY") != nullptr;

/// Called by VideoCore::CreateGPU, after the frontend loaded nx2_switches.txt.
inline void ReadSwitch() {
    g_verify = std::getenv("EDEN_NX2_LIST_HLE_VERIFY") != nullptr;
}

/// A compute macro taken without reading its list words, by TuringMacroInterpreter::CodeHash,
/// and the data RAM words it stores its three parameters in. StoreGridCells must find the same
/// words in its code before a call is taken.
struct HleMacro {
    u64 hash;
    GridCells cells;
};
inline constexpr std::array<HleMacro, 1> HleMacros{{
    {0x055743923e135e09ULL, {0x20, 0x21, 0x22}},
}};

[[nodiscard]] constexpr const HleMacro* FindHleMacro(u64 hash) {
    for (const HleMacro& macro : HleMacros) {
        if (macro.hash == hash) {
            return &macro;
        }
    }
    return nullptr;
}

// A marker word: 0x7E5 in bits 20-31, the word it stands for (0 x, 1 y, 2 z) in bits 16-17 and
// the low 16 bits of the list's keep id below, never 0 (NextKeepId). No device dispatches 0x7E5
// << 20 groups, and the QMD scheduler sees nonzero grid words.
inline constexpr u32 MarkerBase = 0x7E500000;
inline constexpr u32 MarkerMask = 0xFFFC0000;

[[nodiscard]] constexpr u32 Marker(u64 keep_id, u32 index) {
    return MarkerBase | (index << 16) | static_cast<u32>(keep_id & 0xFFFF);
}

[[nodiscard]] constexpr bool IsMarker(u32 value) {
    return (value & MarkerMask) == MarkerBase && ((value >> 16) & 3) != 3 && (value & 0xFFFF) != 0;
}

[[nodiscard]] constexpr u32 MarkerIndex(u32 marker) {
    return (marker >> 16) & 3;
}

[[nodiscard]] constexpr u32 MarkerKeepBits(u32 marker) {
    return marker & 0xFFFF;
}

/// The back's kept-words slots: a list's words are kept in slot keep_id % KeptSlots until a later
/// list takes the slot (ClaimSlot). It divides 2^16, so a marker's keep bits name the slot.
inline constexpr u32 KeptSlots = 1024;
static_assert(0x10000 % KeptSlots == 0);

/// Who took the list a kept slot holds. Front only: the split has one front thread.
struct SlotOwner {
    u64 keep_id{};
    /// Before the slot is reused: gives the list's live markers their real words. False when it
    /// can't yet (an upload still on its way carries them), so the slot must not be reused.
    bool (*evict)(void* context, u64 keep_id){};
    void* context{};
};
inline std::array<SlotOwner, KeptSlots> g_slot_owners{};

/// Takes the slot of `keep_id` for `owner`, after the list it holds was evicted. False, with
/// nothing changed, if that list can't be evicted now.
[[nodiscard]] inline bool ClaimSlot(const SlotOwner& owner) {
    SlotOwner& slot = g_slot_owners[owner.keep_id % KeptSlots];
    if (slot.keep_id != 0 && slot.keep_id != owner.keep_id && slot.evict != nullptr &&
        !slot.evict(slot.context, slot.keep_id)) {
        return false;
    }
    slot = owner;
    return true;
}

/// Forgets the slots `context` took (its engine is destroyed).
inline void ReleaseSlots(const void* context) {
    for (SlotOwner& slot : g_slot_owners) {
        if (slot.context == context) {
            slot = {};
        }
    }
}

/// Keep ids are shared by every channel; none has 0 in its low 16 bits.
inline std::atomic<u64> g_next_keep_id{1};

[[nodiscard]] inline u64 NextKeepId() {
    u64 id;
    do {
        id = g_next_keep_id.fetch_add(1, std::memory_order_relaxed);
    } while ((id & 0xFFFF) == 0);
    return id;
}

namespace Detail {

/// A value the store macro computes: a constant of its code, one of its parameters unchanged, or
/// anything else.
struct Value {
    bool known{};
    u32 value{};
    int parameter{-1};
};

/// The ALU result of `op` on two constants, as TuringMacroInterpreter::Execute computes it. False
/// for the operations a store macro may not use.
[[nodiscard]] inline bool Constant(u32 op, u32 x, u32 y, u32 imm, u32& carry, bool& carry_known,
                                   u32& out) {
    switch (op) {
    case 0:
        out = x + y;
        carry = out < x;
        carry_known = true;
        return true;
    case 1:
        if (!carry_known) {
            return false;
        }
        out = x + y + carry;
        return true;
    case 2:
        out = x - y;
        carry = out > x;
        carry_known = true;
        return true;
    case 3:
        if (!carry_known) {
            return false;
        }
        out = x - y - carry;
        return true;
    case 9:
        out = x << (y & 31);
        return true;
    case 10:
        out = x >> (y & 31);
        return true;
    case 11:
        out = static_cast<u32>(static_cast<s32>(x) >> (y & 31));
        return true;
    case 12:
        out = x & y;
        return true;
    case 13:
        out = ~(x & y);
        return true;
    case 14:
        out = x | y;
        return true;
    case 15:
        out = x ^ y;
        return true;
    case 16: {
        const u32 dest = (imm >> 10) & 63, count = (imm >> 5) & 31, source = imm & 31;
        if (dest >= 32) {
            return false;
        }
        const u32 mask = (1U << count) - 1;
        out = (x & ~(mask << dest)) | (((y >> source) & mask) << dest);
        return true;
    }
    default:
        return false;
    }
}

} // namespace Detail

/// If the macro at `start` of `code` does nothing but store its three parameters, unchanged, in
/// three data RAM words: those words, in parameter order. The macro is followed value by value,
/// not run on samples: a parameter may only be moved (an addition of a constant zero of the code)
/// and stored; any operation on it, any store of another value, any method write or branch, and
/// the store addresses must be constants of the code. Proven for every parameter value (#1336).
[[nodiscard]] inline std::optional<GridCells> StoreGridCells(std::span<const u32> code, u32 start) {
    using Detail::Value;
    constexpr u32 DataRamWrite = 31;
    const u32 instructions = static_cast<u32>(code.size() / 3);
    std::array<Value, 24> regs{};
    std::array<bool, 24> initialized{};
    u32 carry{};
    bool carry_known = true;
    int consumed = 0;
    GridCells cells{~0U, ~0U, ~0U};
    int stores = 0;
    bool ended = false;
    for (u32 pc = start;; ++pc) {
        if (pc >= instructions || pc - start > 16) {
            return std::nullopt;
        }
        const auto d =
            TuringMacroInterpreter::Decode({code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
        // One path whatever the parameters are: no predicate, branch, loop, register or data RAM
        // read.
        const auto allowed = [](u32 op) { return (op < 22 && op != 7) || op == DataRamWrite; };
        if (d.pred_mode != 0 || !allowed(d.op[0]) || !allowed(d.op[1])) {
            return std::nullopt;
        }
        std::array<Value, 2> load{};
        for (u32 i = 0; i < 2; ++i) {
            if (d.load[i]) {
                if (consumed >= 3) {
                    return std::nullopt;
                }
                load[i] = {false, 0, consumed++};
            }
        }
        const auto get = [&](u32 reg, u32 lane) -> std::optional<Value> {
            if (reg < 24) {
                if (!initialized[reg]) {
                    return std::nullopt;
                }
                return regs[reg];
            }
            switch (reg) {
            case 24:
                return Value{true, 0, -1};
            case 25:
                return Value{true, static_cast<u32>(static_cast<s32>(static_cast<s16>(d.imm[lane]))),
                             -1};
            case 26:
                return Value{
                    true, static_cast<u32>(static_cast<s32>(static_cast<s16>(d.imm[1 - lane]))), -1};
            case 27:
                return Value{true, (d.imm[0] << 16) | d.imm[1], -1};
            case 28:
                return load[0];
            case 29:
                return load[1];
            default:
                return std::nullopt;
            }
        };
        std::array<Value, 2> results{};
        for (u32 lane = 0; lane < 2; ++lane) {
            const auto x = get(d.src_a[lane], lane);
            const auto y = get(d.src_b[lane], lane);
            if (!x || !y) {
                return std::nullopt;
            }
            const u32 op = d.op[lane];
            if (op == DataRamWrite) {
                // The address a constant, the value one parameter as it is, each stored once.
                if (!x->known || y->parameter < 0 || cells[y->parameter] != ~0U ||
                    std::ranges::find(cells, x->value) != cells.end()) {
                    return std::nullopt;
                }
                cells[y->parameter] = x->value;
                ++stores;
                results[lane] = {};
            } else if (op == 0 &&
                       ((x->parameter >= 0 && y->known && y->value == 0 && y->parameter < 0) ||
                        (y->parameter >= 0 && x->known && x->value == 0 && x->parameter < 0))) {
                // A move of a parameter.
                results[lane] = x->parameter >= 0 ? *x : *y;
                carry = 0;
                carry_known = true;
            } else if (x->known && y->known) {
                u32 value{};
                if (!Detail::Constant(op, x->value, y->value, d.imm[lane], carry, carry_known,
                                      value)) {
                    return std::nullopt;
                }
                results[lane] = {true, value, -1};
            } else {
                // Anything computed from a parameter: never stored, never an address.
                results[lane] = {false, 0, -1};
                if (op == 0 || op == 2) {
                    carry_known = false;
                }
            }
        }
        for (u32 lane = 0; lane < 2; ++lane) {
            if (d.dst[lane] < 24) {
                regs[d.dst[lane]] = results[lane];
                initialized[d.dst[lane]] = true;
            } else if (d.dst[lane] != 24) {
                return std::nullopt;
            }
            if (d.out_emit[lane] != 0) {
                return std::nullopt;
            }
        }
        if (ended) {
            break;
        }
        ended = d.end;
    }
    if (consumed != 3 || stores != 3) {
        return std::nullopt;
    }
    return cells;
}

/// Where the macro at `start`, whose code hashes to `hash`, stores the grid it takes from a list:
/// its HleMacros entry's words, if its code stores its parameters exactly there.
[[nodiscard]] inline std::optional<GridCells> RecognizeHleMacro(std::span<const u32> code,
                                                                u32 start, u64 hash) {
    const HleMacro* const macro = FindHleMacro(hash);
    if (macro == nullptr) {
        return std::nullopt;
    }
    const auto cells = StoreGridCells(code, start);
    if (!cells || *cells != macro->cells) {
        return std::nullopt;
    }
    return cells;
}

/// Reads a list's three words back after everything sent before (DmaPusher::ReadListWords).
struct ListReader {
    bool (*read)(void* context, GPUVAddr address, u64 keep_id, std::span<u32> words){};
    void* context{};
};

/// A list a channel took, for reading its words back.
struct KeptList {
    u64 keep_id{};
    GPUVAddr address{};
    u64 hash{}; ///< The macro the list fed
    ListReader reader{};
    bool resolved{}; ///< `words` are the list's, read back
    std::array<u32, 3> words{};
    bool verified{}; ///< `real` are the list's, read the old way (verify mode)
    std::array<u32, 3> real{};
};

/// The markers a channel's data RAM holds, shared by its 3D and compute engines (which share the
/// data RAM), and the lists taken last, which markers copied elsewhere stand for.
struct GridMarker {
    GridCells cells{};
    u64 keep_id{}; ///< The list whose markers `cells` were set to
    bool set{};
    std::array<KeptList, KeptSlots> recent{}; ///< By slot: each list whose slot is not reused

    void Remember(const KeptList& list) {
        recent[list.keep_id % recent.size()] = list;
    }

    /// The list `marker` stands for, if it is still among the recent ones.
    [[nodiscard]] KeptList* Find(u32 marker) {
        KeptList& list = recent[MarkerKeepBits(marker) % recent.size()];
        return list.keep_id != 0 && (list.keep_id & 0xFFFF) == MarkerKeepBits(marker) ? &list
                                                                                       : nullptr;
    }

    /// Whether cell `i` of `ram` still holds its marker (a macro may have stored over it).
    template <typename Ram>
    [[nodiscard]] bool Holds(const Ram& ram, size_t i) const {
        return set && ram[cells[i]] == Marker(keep_id, static_cast<u32>(i));
    }

    template <typename Ram>
    [[nodiscard]] std::array<bool, 3> Marked(const Ram& ram) const {
        return {Holds(ram, 0), Holds(ram, 1), Holds(ram, 2)};
    }

    template <typename Ram>
    [[nodiscard]] bool In(const Ram& ram) const {
        return Holds(ram, 0) || Holds(ram, 1) || Holds(ram, 2);
    }
};

/// The real word `marker` stands for, read back once per list. Nothing if the list is no longer
/// known or cannot be read: the caller stores 0, never the marker.
[[nodiscard]] inline std::optional<u32> ResolveWord(GridMarker& state, u32 marker) {
    KeptList* const list = state.Find(marker);
    if (list == nullptr || MarkerIndex(marker) > 2) {
        return std::nullopt;
    }
    if (!list->resolved) {
        if (list->reader.read == nullptr ||
            !list->reader.read(list->reader.context, list->address, list->keep_id, list->words)) {
            return std::nullopt;
        }
        list->resolved = true;
    }
    return list->words[MarkerIndex(marker)];
}

/// Gives each data RAM word that still holds its marker the real word. Returns how many it
/// resolved. Only a list that can't be read at all (unmapped, no kept copy) leaves 0.
template <typename Ram>
u32 ResolveCells(GridMarker& state, Ram& ram) {
    u32 resolved = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (state.Holds(ram, i)) {
            ram[state.cells[i]] = ResolveWord(state, Marker(state.keep_id, i)).value_or(0);
            ++resolved;
        }
    }
    return resolved;
}

/// Before the slot of list `keep_id` is reused: the data RAM words that still hold its markers
/// get their real words (read while the slot still holds them). Returns how many.
template <typename Ram>
u32 EvictFromRam(GridMarker& state, Ram& ram, u64 keep_id) {
    return state.keep_id == keep_id ? ResolveCells(state, ram) : 0;
}

/// The compute engine's inline upload as the front follows it: its registers (0x60 to 0x63,
/// 0x6c) and how many data bytes it took since its exec.
struct UploadState {
    u32 line_length{};
    u32 line_count{};
    u64 address{};
    u32 exec{};
    u64 offset{};

    void Apply(u32 method, u32 value) {
        switch (method) {
        case 0x60:
            line_length = value;
            break;
        case 0x61:
            line_count = value;
            break;
        case 0x62:
            address = (u64{value} << 32) | (address & 0xffffffff);
            break;
        case 0x63:
            address = (address & ~u64{0xffffffff}) | value;
            break;
        case ExecUploadMethod:
            exec = value;
            offset = 0;
            break;
        case DataUploadMethod:
            offset += 4;
            break;
        default:
            break;
        }
    }
    [[nodiscard]] u64 Size() const {
        return u64{line_length} * line_count;
    }
    /// Data words still to come before the upload is written.
    [[nodiscard]] bool InFlight() const {
        return offset < Size();
    }
    /// Where the next data word lands in memory (a linear upload of one line).
    [[nodiscard]] u64 NextWordAddress() const {
        return address + offset;
    }
    /// Whether the next data word, a marker, lands where the back patches it: in a linear
    /// upload of one line, which Upload::State writes word by word, never twice (a larger one
    /// is written whole on each call); before its end and 4-byte aligned in memory
    /// (RasterizerVulkan::AccelerateInlineToMemory finds aligned words only).
    [[nodiscard]] bool NextWordPatchable() const {
        return (exec & 1) != 0 && line_count == 1 && offset + 4 <= line_length &&
               (NextWordAddress() & 3) == 0;
    }
};

struct Emit {
    Word method;
    Word value;
    bool grid; ///< The value is a marker word, unchanged
};

struct DryRunResult {
    bool completed{}; ///< The call ran to its end as Execute would
    bool safe{};      ///< Nothing but moves and emitted values depend on a marker
    bool read_grid{}; ///< The call read at least one marker
    std::size_t consumed{};
    std::vector<Emit> emits;
    std::vector<std::pair<Word, Word>> data_ram_writes;
    std::string_view reason; ///< Why the call is not completed or not safe
};

/// Runs the macro at `start` of `code` as TuringMacroInterpreter::Execute does, on a copy of
/// `dram`, with `parameters` and register reads through `read`; emits are collected, not applied.
/// `marked` are the data RAM words that hold a marker (GridMarker::Marked of `cells`); a
/// register read is a marker when `is_marker_register(address, value)`. Each marker is followed
/// through registers and outputs: every value is either clean or a marker unchanged. Anything else
/// done with one (arithmetic, a comparison, a branch, a loop count, an address, a method number,
/// a store in the data RAM) makes the call unsafe. Port of #1336's Nx2GridTaint::DryRun.
template <typename Read, typename IsMarkerRegister>
[[nodiscard]] DryRunResult DryRun(std::span<const Word> code, Word start,
                                  std::span<const Word> parameters, const DataRam& dram,
                                  const GridCells& cells, const std::array<bool, 3>& marked,
                                  Read read, IsMarkerRegister is_marker_register) {
    using Decoded = TuringMacroInterpreter::Decoded;
    DryRunResult result;
    DataRam ram = dram;
    // Each cell while it holds its marker: a word a macro stored there itself is a value like
    // any other.
    std::array<bool, 3> grid_cell = marked;
    const auto cell_index = [&](Word address) -> int {
        for (int i = 0; i < 3; ++i) {
            if (cells[i] == address) {
                return i;
            }
        }
        return -1;
    };
    std::array<Word, 24> regs{};
    std::array<bool, 24> reg_grid{};
    Word initialized{}, carry{}, method{}, increment{};
    Word pc = start, loop_count{}, loop_start{}, loop_end{};
    bool end_next{}, stopped{};
    result.safe = true;
    bool failed = false;
    const auto stop = [&](std::string_view reason, bool unsafe) {
        if (result.reason.empty()) {
            result.reason = reason;
        }
        result.safe = result.safe && !unsafe;
        failed = true;
    };
    const auto fail = [&](std::string_view reason) { stop(reason, false); };
    const auto unsafe = [&](std::string_view reason) { stop(reason, true); };
    for (Word steps = 0; steps < 1000000 && !failed; ++steps) {
        if (static_cast<std::size_t>(pc) * 3 + 2 >= code.size()) {
            fail("instruction outside uploaded RAM");
            break;
        }
        const Decoded d =
            TuringMacroInterpreter::Decode({code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
        std::array<Word, 2> load{}, alu{};
        std::array<bool, 2> alu_grid{};
        for (Word i = 0; i < 2 && !failed; ++i) {
            if (d.load[i]) {
                if (result.consumed >= parameters.size()) {
                    // Parameters a memory read would append (MME_DMA_READ_FIFOED) are not known
                    // here either.
                    fail("parameter FIFO underflow");
                    break;
                }
                load[i] = parameters[result.consumed++];
            }
        }
        if (failed) {
            break;
        }
        struct Operand {
            Word value;
            bool grid;
            bool zero_source; ///< A constant zero of the code, not a value that happens to be 0
        };
        const auto get = [&](Word reg, Word lane) -> Operand {
            if (reg < 24) {
                if (!(initialized & (1U << reg))) {
                    fail("uninitialized register read");
                }
                return {regs[reg], reg_grid[reg], false};
            }
            switch (reg) {
            case 24:
                return {0, false, true};
            case 25:
                return {Word(std::int32_t(std::int16_t(d.imm[lane]))), false, d.imm[lane] == 0};
            case 26:
                return {Word(std::int32_t(std::int16_t(d.imm[1 - lane]))), false,
                        d.imm[1 - lane] == 0};
            case 27:
                return {(d.imm[0] << 16) | d.imm[1], false, (d.imm[0] | d.imm[1]) == 0};
            case 28:
                return {load[0], false, false};
            case 29:
                return {load[1], false, false};
            default:
                fail("invalid source register");
                return {0, false, false};
            }
        };
        Word mask = 15;
        if (d.pred_mode) {
            const Operand predicate = get(d.pred_reg, 0);
            if (predicate.grid) {
                unsafe("a predicate tests a marker");
                break;
            }
            mask = predicate.value ? d.mask_true : d.mask_false;
        }
        std::array<Operand, 2> xs{}, ys{};
        for (Word lane = 0; lane < 2; ++lane) {
            if (mask & (1U << lane)) {
                xs[lane] = get(d.src_a[lane], lane);
                ys[lane] = get(d.src_b[lane], lane);
            }
        }
        if (failed) {
            break;
        }
        Word next_pc = pc + 1;
        bool branch_cancels_end{};
        for (Word lane = 0; lane < 2 && !failed; ++lane) {
            if (!(mask & (1U << lane))) {
                continue;
            }
            const Word x = xs[lane].value, y = ys[lane].value;
            const Word imm = d.imm[lane];
            Word value{};
            bool value_grid{};
            const auto condition = [&](Word comparison) {
                switch (comparison) {
                case 17:
                case 25:
                    return std::int32_t(x) < std::int32_t(y);
                case 18:
                case 26:
                    return x < y;
                case 19:
                case 27:
                    return std::int32_t(x) <= std::int32_t(y);
                case 20:
                case 28:
                    return x <= y;
                default:
                    return x == y;
                }
            };
            const Word op = d.op[lane];
            if (xs[lane].grid || ys[lane].grid) {
                // The one thing a marker may go through: an addition with a constant zero, which
                // is how the macro code moves a value.
                const bool move = op == 0 && ((xs[lane].grid && ys[lane].zero_source) ||
                                              (ys[lane].grid && xs[lane].zero_source));
                if (!move) {
                    unsafe(op == 31   ? "a marker is stored in the data RAM"
                           : op == 30 ? "a marker is a data RAM address"
                           : op >= 23 ? "a branch or a loop depends on a marker"
                                      : "an operation takes a marker");
                    break;
                }
                value = x + y;
                carry = 0;
                value_grid = true;
            } else {
                switch (op) {
                case 0:
                    value = x + y;
                    carry = value < x;
                    break;
                case 1:
                    value = x + y + carry;
                    break;
                case 2:
                    value = x - y;
                    carry = value > x;
                    break;
                case 3:
                    value = x - y - carry;
                    break;
                case 4:
                case 6: {
                    const std::uint64_t a =
                        op == 4 ? std::uint64_t(std::int64_t(std::int32_t(x))) : x;
                    const std::uint64_t b =
                        op == 4 ? std::uint64_t(std::int64_t(std::int32_t(y))) : y;
                    const auto product = a * b;
                    value = Word(product);
                    carry = Word(product >> 32);
                    break;
                }
                case 5:
                    value = carry;
                    break;
                case 7:
                    if (x != 0x1000) {
                        fail("unsupported extended operation");
                    }
                    break;
                case 8:
                    value = Word(std::countl_zero(x));
                    break;
                case 9:
                    value = x << (y & 31);
                    break;
                case 10:
                    value = x >> (y & 31);
                    break;
                case 11:
                    value = Word(std::int32_t(x) >> (y & 31));
                    break;
                case 12:
                    value = x & y;
                    break;
                case 13:
                    value = ~(x & y);
                    break;
                case 14:
                    value = x | y;
                    break;
                case 15:
                    value = x ^ y;
                    break;
                case 16: {
                    const Word dest = (imm >> 10) & 63, count = (imm >> 5) & 31;
                    const Word source = imm & 31;
                    if (dest >= 32) {
                        fail("invalid merge destination");
                        break;
                    }
                    const Word bits_mask = (1U << count) - 1;
                    value = (x & ~(bits_mask << dest)) | (((y >> source) & bits_mask) << dest);
                    break;
                }
                case 17:
                case 18:
                case 19:
                case 20:
                case 21:
                    value = condition(op) ? ~0U : 0;
                    break;
                case 22: {
                    // A register this call wrote holds what it emitted there (FrontApply).
                    const Word address = (x + y) & 0xffff;
                    const auto emitted = std::find_if(
                        result.emits.rbegin(), result.emits.rend(),
                        [address](const Emit& emit) { return emit.method == address; });
                    if (emitted != result.emits.rend() && address < 0xe00) {
                        value = emitted->value;
                        value_grid = emitted->grid;
                    } else {
                        value = read(address);
                        value_grid = is_marker_register(address, value);
                    }
                    result.read_grid = result.read_grid || value_grid;
                    break;
                }
                case 23:
                    if (loop_count || imm < 2) {
                        fail("invalid loop");
                        break;
                    }
                    if (x == 0) {
                        next_pc = pc + imm;
                        break;
                    }
                    loop_count = x - 1;
                    loop_start = pc;
                    loop_end = pc + imm - 1;
                    break;
                case 24:
                    next_pc = pc + (imm & 0x7fff);
                    break;
                case 25:
                case 26:
                case 27:
                case 28:
                case 29:
                    if (condition(op) == bool(imm & 0x8000)) {
                        const auto offset = std::int32_t(imm << 19) >> 19;
                        if (offset == -4096) {
                            stopped = true;
                        } else {
                            next_pc = Word(std::int32_t(pc) + offset);
                            branch_cancels_end = (imm & 0x4000) != 0;
                        }
                    }
                    break;
                case 30: {
                    if (x >= ram.size()) {
                        fail("data RAM read out of bounds");
                        break;
                    }
                    value = ram[x];
                    const int index = cell_index(x);
                    if (index >= 0 && grid_cell[index]) {
                        value_grid = true;
                        result.read_grid = true;
                    }
                    break;
                }
                case 31:
                    if (x >= ram.size()) {
                        fail("data RAM write out of bounds");
                        break;
                    }
                    ram[x] = y;
                    result.data_ram_writes.emplace_back(x, y);
                    if (const int index = cell_index(x); index >= 0) {
                        // A value of the macro's own replaces the marker.
                        grid_cell[index] = false;
                    }
                    break;
                default:
                    break;
                }
            }
            if (failed) {
                break;
            }
            alu[lane] = value;
            alu_grid[lane] = value_grid;
            if (d.dst[lane] < 24) {
                regs[d.dst[lane]] = value;
                reg_grid[d.dst[lane]] = value_grid;
                initialized |= 1U << d.dst[lane];
            } else if (d.dst[lane] != 24) {
                fail("invalid destination register");
            }
        }
        if (failed) {
            break;
        }
        struct Output {
            Word value;
            bool grid;
        };
        const auto output = [&](Word selector) -> Output {
            switch (selector) {
            case 1:
                return {alu[0], alu_grid[0]};
            case 2:
                return {alu[1], alu_grid[1]};
            case 3:
                return {load[0], false};
            case 4:
                return {load[1], false};
            case 5:
                return {d.imm[0], false};
            case 6:
                return {d.imm[1], false};
            case 8:
                return {d.imm[0] >> 12, false};
            case 9:
                return {d.imm[1] >> 12, false};
            case 10:
                return {(d.imm[0] << 16) | d.imm[1], false};
            default:
                fail("invalid output selector");
                return {0, false};
            }
        };
        for (Word lane = 0; lane < 2 && !failed; ++lane) {
            if (!(mask & (4U << lane))) {
                continue;
            }
            if (d.out_method[lane]) {
                const Output address = output(d.out_method[lane]);
                if (address.grid) {
                    unsafe("a marker is a method number");
                    break;
                }
                method = address.value & 0xfff;
                increment = (address.value >> 12) & 15;
            }
            if (d.out_emit[lane]) {
                const Output value = output(d.out_emit[lane]);
                if (!failed) {
                    result.emits.push_back({method, value.value, value.grid});
                }
                method += increment;
            }
        }
        if (failed) {
            break;
        }
        if (end_next || stopped) {
            result.completed = true;
            return result;
        }
        end_next = d.end && !branch_cancels_end;
        if (loop_count && pc == loop_end) {
            --loop_count;
            next_pc = loop_start + 1;
        }
        pc = next_pc;
    }
    if (result.reason.empty()) {
        result.reason = "instruction limit exceeded";
    }
    return result;
}

enum class Admission {
    Unaffected, ///< The call reads no marker
    Moves,      ///< It moves markers into upload data or their QMD grid registers only
    Resolve,    ///< It needs the real words first
};

/// Whether a macro call that ran dry as `result` may run on the markers: it completed, used them
/// only as moves, and emitted each as inline upload data the back patches
/// (UploadState::NextWordPatchable) or as the grid register of the QMD word it stands for
/// (0xd4 + word). `upload` is the engine's upload before the call.
[[nodiscard]] inline Admission Admit(const DryRunResult& result, UploadState upload) {
    if (!result.completed || !result.safe) {
        return Admission::Resolve;
    }
    for (const Emit& emit : result.emits) {
        if (emit.grid) {
            const bool patched = emit.method == DataUploadMethod && upload.NextWordPatchable();
            if (!patched && emit.method != QmdGridRegister + MarkerIndex(emit.value)) {
                return Admission::Resolve;
            }
        }
        upload.Apply(emit.method, emit.value);
    }
    return result.read_grid ? Admission::Moves : Admission::Unaffected;
}

/// A marker an admitted call puts in inline upload data.
struct UploadMarker {
    std::size_t emit; ///< Which of the call's emits writes it
    u64 address;      ///< Where it lands in memory
    u32 marker;
    bool operator==(const UploadMarker&) const = default;
};

/// The markers an admitted call (Admit: Moves) puts in inline upload data. Data that only equals
/// a marker is not among them.
[[nodiscard]] inline std::vector<UploadMarker> UploadMarkerWords(const DryRunResult& result,
                                                                UploadState upload) {
    std::vector<UploadMarker> markers;
    for (std::size_t i = 0; i < result.emits.size(); ++i) {
        const Emit& emit = result.emits[i];
        if (emit.grid && emit.method == DataUploadMethod) {
            markers.push_back({i, upload.NextWordAddress(), emit.value});
        }
        upload.Apply(emit.method, emit.value);
    }
    return markers;
}

/// A marker an inline write carries, and the list it stands for.
struct KeptPatch {
    u32 offset; ///< Byte offset in the write
    u64 keep_id;
    u32 index; ///< The list word it stands for
};

/// The words of an inline write of `data` to device address `address` the back copies kept
/// words over: those the front said would carry a marker there (`expected(address)`: keep id
/// and word, RasterizerInterface::Nx2ExpectKeptWord) and that hold exactly that marker. A data
/// word that only looks like a marker (a buffer address can) is never patched.
template <typename Expected>
[[nodiscard]] std::vector<KeptPatch> PlanPatches(u64 address, std::span<const u8> data,
                                                 Expected&& expected) {
    std::vector<KeptPatch> patches;
    for (size_t offset = (4 - (address & 3)) & 3; offset + 4 <= data.size(); offset += 4) {
        u32 word;
        std::memcpy(&word, data.data() + offset, sizeof(word));
        if (!IsMarker(word)) {
            continue;
        }
        const std::optional<std::pair<u64, u32>> marker = expected(address + offset);
        if (marker && word == Marker(marker->first, marker->second)) {
            patches.push_back({static_cast<u32>(offset), marker->first, marker->second});
        }
    }
    return patches;
}

enum class GridKind {
    Direct,   ///< No marker: the QMD's own grid
    Indirect, ///< A list's three markers in order: dispatched from its kept copy
    ReadBack, ///< Markers mixed with other words: the kept words are read back
};

struct GridSource {
    GridKind kind;
    u32 keep_bits{};
};

/// The list a QMD grid word `marker` stands for: the list whose word the back patched at its
/// address (`registered`, BufferCache::Nx2KeptWordList: keep id and word), if that is the
/// marker's word of that list and the marker's slot still holds the list (`kept`). The marker's
/// 16 bits of keep id alone are never trusted: they wrap. Nothing otherwise, and the words come
/// from the host GPU.
[[nodiscard]] constexpr std::optional<u64> QmdWordList(
    u32 marker, std::optional<std::pair<u64, u32>> registered, std::optional<u64> kept) {
    if (!IsMarker(marker) || !registered || registered->second != MarkerIndex(marker) ||
        (registered->first & 0xFFFF) != MarkerKeepBits(marker) || kept != registered->first) {
        return std::nullopt;
    }
    return registered->first;
}

/// Where the grid of a QMD whose words 12 to 14 are `x`, `y` and `z` comes from.
[[nodiscard]] constexpr GridSource QmdGridSource(u32 x, u32 y, u32 z) {
    if (!IsMarker(x) && !IsMarker(y) && !IsMarker(z)) {
        return {GridKind::Direct};
    }
    if (IsMarker(x) && IsMarker(y) && IsMarker(z) && MarkerIndex(x) == 0 && MarkerIndex(y) == 1 &&
        MarkerIndex(z) == 2 && MarkerKeepBits(x) == MarkerKeepBits(y) &&
        MarkerKeepBits(y) == MarkerKeepBits(z)) {
        return {GridKind::Indirect, MarkerKeepBits(x)};
    }
    return {GridKind::ReadBack};
}

/// Verify mode: whether `marked`, the emits of a run on the markers, match `real`, the same
/// call's emits on the real words: the same writes, where each marker the run moved
/// (Emit::grid) is the word `real_word(marker)` gives. A marker without a real word never
/// matches; any other value compares as it is.
template <typename RealWord>
[[nodiscard]] bool MatchesRealRun(std::span<const Emit> marked, std::span<const Emit> real,
                                  RealWord&& real_word) {
    if (marked.size() != real.size()) {
        return false;
    }
    for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i].method != real[i].method) {
            return false;
        }
        Word value = marked[i].value;
        if (marked[i].grid) {
            const std::optional<Word> word = real_word(value);
            if (!word) {
                return false;
            }
            value = *word;
        }
        if (value != real[i].value) {
            return false;
        }
    }
    return true;
}

/// Verify mode checks, each counted by the hash of the macro it is about.
enum class Check : u8 {
    Kept,   ///< The kept copy against the words read the old way (store macro)
    Reader, ///< A guarded macro's run on the markers against its run on the real words (reader)
    Grid,   ///< An indirect dispatch's grid against the QMD grid the real words give (store macro)
    Upload, ///< A patched upload word against the real word (store macro)
    Forced, ///< A marker resolved before its kept slot was reused, against the real word
    Count,
};

/// Counts one verify check; logs the first 16 mismatches with `detail`, and a summary every 10 s.
void NoteVerify(Check check, u64 hash, bool match, std::string_view detail = {});

} // namespace Tegra::Nx2ListGrid
