// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
/// Address in constant buffers to the storage buffer descriptor
struct StorageBufferAddr {
    auto operator<=>(const StorageBufferAddr&) const noexcept = default;

    u32 index;
    u32 offset;
    /// Offset of the buffer pointer in the memory the constant buffer pointer points to
    u32 indirect{~0U};
};

/// Block iterator to a global memory instruction and the storage buffer it uses
struct StorageInst {
    StorageBufferAddr storage_buffer;
    IR::Inst* inst;
    IR::Block* block;
};

/// Bias towards a certain range of constant buffers when looking for storage buffers
struct Bias {
    u32 index;
    u32 offset_begin;
    u32 offset_end;
    u32 alignment;
};

using boost::container::flat_set;
using boost::container::small_vector;
using StorageBufferSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;
using StorageInstVector = small_vector<StorageInst, 24>;
using StorageWritesSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;

struct StorageInfo {
    StorageBufferSet set;
    StorageInstVector to_replace;
    StorageWritesSet writes;
    /// Constant buffer words that step strided iterators, see CollectIteratorWords
    StorageBufferSet iterator_words;
    /// Constant buffer pointers a global atomic uses as its address, see CollectAtomicPointers
    StorageBufferSet atomic_pointers;
    /// Track results by the instruction the search starts from, with and without the NVN bias:
    /// accesses through the same address share them, and a search can walk thousands of
    /// instructions (the IR does not change while storage buffers are collected).
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> biased_tracks;
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> unbiased_tracks;
    std::unordered_map<const IR::Inst*, small_vector<StorageBufferAddr, 4>> native_candidates;
    /// Native accesses whose candidates include slots of a dynamically indexed pointer array.
    std::unordered_set<const IR::Inst*> native_dynamic;
    /// A native access with several candidate pointers was left global.
    bool native_runtime_bounds{};
    /// Pointer table entries reached through a phi or select of table pointers, or through a
    /// table pointer read from a dynamically indexed constant buffer array. They are bound only
    /// in the bindings the program's other storage buffers leave free (table_budget).
    bool tables_enabled{};
    bool tables_seen{};
    size_t table_budget{};
    size_t table_used{};
    small_vector<StorageBufferAddr, 8> table_candidates;
};

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemory(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemoryWrite(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Converts a global memory opcode to its storage buffer equivalent
IR::Opcode GlobalToStorage(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalS8:
        return IR::Opcode::LoadStorageS8;
    case IR::Opcode::LoadGlobalU8:
        return IR::Opcode::LoadStorageU8;
    case IR::Opcode::LoadGlobalS16:
        return IR::Opcode::LoadStorageS16;
    case IR::Opcode::LoadGlobalU16:
        return IR::Opcode::LoadStorageU16;
    case IR::Opcode::LoadGlobal32:
        return IR::Opcode::LoadStorage32;
    case IR::Opcode::LoadGlobal64:
        return IR::Opcode::LoadStorage64;
    case IR::Opcode::LoadGlobal128:
        return IR::Opcode::LoadStorage128;
    case IR::Opcode::WriteGlobalS8:
        return IR::Opcode::WriteStorageS8;
    case IR::Opcode::WriteGlobalU8:
        return IR::Opcode::WriteStorageU8;
    case IR::Opcode::WriteGlobalS16:
        return IR::Opcode::WriteStorageS16;
    case IR::Opcode::WriteGlobalU16:
        return IR::Opcode::WriteStorageU16;
    case IR::Opcode::WriteGlobal32:
        return IR::Opcode::WriteStorage32;
    case IR::Opcode::WriteGlobal64:
        return IR::Opcode::WriteStorage64;
    case IR::Opcode::WriteGlobal128:
        return IR::Opcode::WriteStorage128;
    case IR::Opcode::GlobalAtomicIAdd32:
        return IR::Opcode::StorageAtomicIAdd32;
    case IR::Opcode::GlobalAtomicSMin32:
        return IR::Opcode::StorageAtomicSMin32;
    case IR::Opcode::GlobalAtomicUMin32:
        return IR::Opcode::StorageAtomicUMin32;
    case IR::Opcode::GlobalAtomicSMax32:
        return IR::Opcode::StorageAtomicSMax32;
    case IR::Opcode::GlobalAtomicUMax32:
        return IR::Opcode::StorageAtomicUMax32;
    case IR::Opcode::GlobalAtomicInc32:
        return IR::Opcode::StorageAtomicInc32;
    case IR::Opcode::GlobalAtomicDec32:
        return IR::Opcode::StorageAtomicDec32;
    case IR::Opcode::GlobalAtomicAnd32:
        return IR::Opcode::StorageAtomicAnd32;
    case IR::Opcode::GlobalAtomicOr32:
        return IR::Opcode::StorageAtomicOr32;
    case IR::Opcode::GlobalAtomicXor32:
        return IR::Opcode::StorageAtomicXor32;
    case IR::Opcode::GlobalAtomicExchange32:
        return IR::Opcode::StorageAtomicExchange32;
    case IR::Opcode::GlobalAtomicIAdd64:
        return IR::Opcode::StorageAtomicIAdd64;
    case IR::Opcode::GlobalAtomicSMin64:
        return IR::Opcode::StorageAtomicSMin64;
    case IR::Opcode::GlobalAtomicUMin64:
        return IR::Opcode::StorageAtomicUMin64;
    case IR::Opcode::GlobalAtomicSMax64:
        return IR::Opcode::StorageAtomicSMax64;
    case IR::Opcode::GlobalAtomicUMax64:
        return IR::Opcode::StorageAtomicUMax64;
    case IR::Opcode::GlobalAtomicAnd64:
        return IR::Opcode::StorageAtomicAnd64;
    case IR::Opcode::GlobalAtomicOr64:
        return IR::Opcode::StorageAtomicOr64;
    case IR::Opcode::GlobalAtomicXor64:
        return IR::Opcode::StorageAtomicXor64;
    case IR::Opcode::GlobalAtomicExchange64:
        return IR::Opcode::StorageAtomicExchange64;
    case IR::Opcode::GlobalAtomicIAdd32x2:
        return IR::Opcode::StorageAtomicIAdd32x2;
    case IR::Opcode::GlobalAtomicSMin32x2:
        return IR::Opcode::StorageAtomicSMin32x2;
    case IR::Opcode::GlobalAtomicUMin32x2:
        return IR::Opcode::StorageAtomicUMin32x2;
    case IR::Opcode::GlobalAtomicSMax32x2:
        return IR::Opcode::StorageAtomicSMax32x2;
    case IR::Opcode::GlobalAtomicUMax32x2:
        return IR::Opcode::StorageAtomicUMax32x2;
    case IR::Opcode::GlobalAtomicAnd32x2:
        return IR::Opcode::StorageAtomicAnd32x2;
    case IR::Opcode::GlobalAtomicOr32x2:
        return IR::Opcode::StorageAtomicOr32x2;
    case IR::Opcode::GlobalAtomicXor32x2:
        return IR::Opcode::StorageAtomicXor32x2;
    case IR::Opcode::GlobalAtomicExchange32x2:
        return IR::Opcode::StorageAtomicExchange32x2;
    case IR::Opcode::GlobalAtomicAddF32:
        return IR::Opcode::StorageAtomicAddF32;
    case IR::Opcode::GlobalAtomicAddF16x2:
        return IR::Opcode::StorageAtomicAddF16x2;
    case IR::Opcode::GlobalAtomicMinF16x2:
        return IR::Opcode::StorageAtomicMinF16x2;
    case IR::Opcode::GlobalAtomicMaxF16x2:
        return IR::Opcode::StorageAtomicMaxF16x2;
    case IR::Opcode::GlobalAtomicAddF32x2:
        return IR::Opcode::StorageAtomicAddF32x2;
    case IR::Opcode::GlobalAtomicMinF32x2:
        return IR::Opcode::StorageAtomicMinF32x2;
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return IR::Opcode::StorageAtomicMaxF32x2;
    default:
        throw InvalidArgument("Invalid global memory opcode {}", opcode);
    }
}

/// Returns true when a storage buffer address satisfies a bias
bool MeetsBias(const StorageBufferAddr& storage_buffer, const Bias& bias) noexcept {
    return storage_buffer.index == bias.index && storage_buffer.offset >= bias.offset_begin &&
           storage_buffer.offset < bias.offset_end;
}

struct LowAddrInfo {
    IR::U32 value;
    IR::U32 high;
    s32 imm_offset;
};

/// Tries to track the first 32-bits of a global memory instruction
std::optional<LowAddrInfo> TrackLowAddress(IR::Inst* inst) {
    // The first argument is the low level GPU pointer to the global memory instruction
    const IR::Value addr{inst->Arg(0)};
    if (addr.IsImmediate()) {
        // Not much we can do if it's an immediate
        return std::nullopt;
    }
    // This address is expected to either be a PackUint2x32, a IAdd64, or a CompositeConstructU32x2
    IR::Inst* addr_inst{addr.InstRecursive()};
    s32 imm_offset{0};
    if (addr_inst->GetOpcode() == IR::Opcode::IAdd64) {
        // If it's an IAdd64, get the immediate offset it is applying and grab the address
        // instruction. This expects for the instruction to be canonicalized having the address on
        // the first argument and the immediate offset on the second one.
        const IR::U64 imm_offset_value{addr_inst->Arg(1)};
        if (!imm_offset_value.IsImmediate()) {
            return std::nullopt;
        }
        imm_offset = static_cast<s32>(static_cast<s64>(imm_offset_value.U64()));
        const IR::U64 iadd_addr{addr_inst->Arg(0)};
        if (iadd_addr.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = iadd_addr.InstRecursive();
    }
    // With IAdd64 handled, now PackUint2x32 is expected
    if (addr_inst->GetOpcode() == IR::Opcode::PackUint2x32) {
        // PackUint2x32 is expected to be generated from a vector
        const IR::Value vector{addr_inst->Arg(0)};
        if (vector.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = vector.InstRecursive();
    }
    // The vector is expected to be a CompositeConstructU32x2
    if (addr_inst->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
        return std::nullopt;
    }
    // Grab the first argument from the CompositeConstructU32x2, this is the low address.
    return LowAddrInfo{
        .value{IR::U32{addr_inst->Arg(0)}},
        .high{IR::U32{addr_inst->Arg(1)}},
        .imm_offset = imm_offset,
    };
}

/// Tries to track the storage buffer address used by a global memory instruction
std::optional<StorageBufferAddr> Track(const IR::Value& value, const Bias* bias) {
    const auto pred{[bias](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 &&
            inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate()) {
            // Definitely not a storage buffer if it's read from a
            // non-immediate index
            return std::nullopt;
        }
        if (!offset.IsImmediate()) {
            // TODO: Support SSBO arrays
            return std::nullopt;
        }
        const StorageBufferAddr storage_buffer{
            .index = index.U32(),
            .offset = offset.U32(),
        };
        const u32 alignment{bias ? bias->alignment : 8U};
        if (!Common::IsAligned(storage_buffer.offset, alignment)) {
            // The SSBO pointer has to be aligned
            return std::nullopt;
        }
        if (bias && !MeetsBias(storage_buffer, *bias)) {
            // We have to blacklist some addresses in case we wrongly
            // point to them
            return std::nullopt;
        }
        return storage_buffer;
    }};
    // A pointer loaded from memory is a new address, not an offset into the buffer that supplied
    // it. Do not follow that load back to its own constant-buffer descriptor.
    const auto stop_at_memory = [](IR::Value operand) {
        if (!operand.IsImmediate() && IsGlobalMemory(*operand.InstRecursive())) {
            return IR::Value{u32{0}};
        }
        return operand;
    };
    return BreadthFirstSearch(value, pred, stop_at_memory);
}

/// Finds the one constant buffer word added directly into the low address, which is the base
/// pointer even when other constant buffer reads feed the computed offset
std::optional<StorageBufferAddr> TrackAddend(const IR::Value& value) {
    std::optional<StorageBufferAddr> result;
    bool ambiguous{};
    const auto visit{[&](const auto& self, const IR::Value& term) -> void {
        if (term.IsImmediate() || ambiguous) {
            return;
        }
        const IR::Inst* const inst{term.InstRecursive()};
        if (inst->GetOpcode() == IR::Opcode::IAdd32) {
            self(self, inst->Arg(0));
            self(self, inst->Arg(1));
            return;
        }
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 || !inst->Arg(0).IsImmediate() ||
            !inst->Arg(1).IsImmediate() || !Common::IsAligned(inst->Arg(1).U32(), 8)) {
            return;
        }
        const StorageBufferAddr found{.index = inst->Arg(0).U32(), .offset = inst->Arg(1).U32()};
        ambiguous = result.has_value();
        result = found;
    }};
    visit(visit, value);
    return ambiguous ? std::nullopt : result;
}

/// Returns the constant buffer word an instruction reads, including one half of a 64-bit read
std::optional<StorageBufferAddr> CbufWord(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::CompositeExtractU32x2 && !inst->Arg(0).IsImmediate()) {
        const IR::Inst* const vector{inst->Arg(0).InstRecursive()};
        if (vector->GetOpcode() != IR::Opcode::GetCbufU32x2 || !vector->Arg(0).IsImmediate() ||
            !vector->Arg(1).IsImmediate()) {
            return std::nullopt;
        }
        return StorageBufferAddr{
            .index = vector->Arg(0).U32(),
            .offset = vector->Arg(1).U32() + inst->Arg(1).U32() * 4,
        };
    }
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32 || !inst->Arg(0).IsImmediate() ||
        !inst->Arg(1).IsImmediate()) {
        return std::nullopt;
    }
    return StorageBufferAddr{.index = inst->Arg(0).U32(), .offset = inst->Arg(1).U32()};
}

struct TrackingConditions {
    using Predicate = std::pair<IR::Value, bool>;

    struct Scope {
        Predicate condition;
        IR::Block* header;
        IR::Block* merge;
    };
    std::unordered_map<const IR::Inst*, IR::Block*> owners;
    std::unordered_map<IR::Block*, std::vector<Scope>> scopes;
    std::unordered_map<IR::Block*, std::vector<Scope>> merges;
    std::unordered_map<IR::Block*, IR::Block*> loops;
    IR::Block* current{};
    using BitKey = std::tuple<const IR::Inst*, u32, const IR::Inst*, int>;
    mutable std::map<BitKey, std::optional<Predicate>> bit_cache;

    explicit TrackingConditions(IR::Program& program) {
        for (IR::Block* block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Phi) {
                    owners.emplace(&inst, block);
                }
            }
        }
        std::vector<Scope> active;
        std::vector<IR::Block*> loop_stack;
        IR::Block* previous{};
        for (const auto& node : program.syntax_list) {
            switch (node.type) {
            case IR::AbstractSyntaxNode::Type::Block:
                previous = node.data.block;
                scopes[previous] = active;
                loops[previous] = loop_stack.empty() ? nullptr : loop_stack.back();
                break;
            case IR::AbstractSyntaxNode::Type::If: {
                bit_cache.clear();
                const Scope scope{Literal(node.data.if_node.cond), previous,
                                  node.data.if_node.merge};
                merges[scope.merge].push_back(scope);
                active.push_back(scope);
                break;
            }
            case IR::AbstractSyntaxNode::Type::EndIf:
                active.pop_back();
                break;
            case IR::AbstractSyntaxNode::Type::Loop:
                loop_stack.push_back(node.data.loop.body);
                break;
            case IR::AbstractSyntaxNode::Type::Repeat:
                loop_stack.pop_back();
                break;
            default:
                break;
            }
        }
    }

    static std::optional<bool> Constant(Predicate value) {
        if (value.first.IsImmediate() && value.first.Type() == IR::Type::U1) {
            return value.first.U1() == value.second;
        }
        return std::nullopt;
    }

    std::optional<Predicate> Bit(IR::Value value, u32 bit, u32 depth,
                                 std::optional<Predicate> assumed = {}) const {
        if (value.IsImmediate())
            return BitImpl(value, bit, depth, assumed);
        const BitKey key{
            value.InstRecursive(), bit, assumed ? assumed->first.TryInstRecursive() : nullptr,
            assumed
                ? (Constant(*assumed).has_value() ? 4 + *Constant(*assumed) : 1 + assumed->second)
                : 0};
        if (const auto found = bit_cache.find(key); found != bit_cache.end())
            return found->second;
        bit_cache[key] = std::nullopt;
        const auto result = BitImpl(value, bit, depth, assumed);
        bit_cache[key] = result;
        return result;
    }

    std::optional<Predicate> BitImpl(IR::Value value, u32 bit, u32 depth,
                                     std::optional<Predicate> assumed) const {
        if (bit >= 32 || depth == 0) {
            return std::nullopt;
        }
        if (value.IsImmediate()) {
            return Predicate{IR::Value{((value.U32() >> bit) & 1) != 0}, true};
        }
        const auto* inst = value.InstRecursive();
        if (inst->GetOpcode() == IR::Opcode::Phi && inst->NumArgs() == 2) {
            const auto owner = owners.find(inst);
            if (owner != owners.end()) {
                const auto merge = merges.find(owner->second);
                if (merge != merges.end() && merge->second.size() == 1) {
                    const auto& branch = merge->second.front();
                    size_t bypass = inst->PhiBlock(0) == branch.header ? 0 : 1;
                    if (inst->PhiBlock(bypass) == branch.header &&
                        inst->PhiBlock(1 - bypass)->GetOrder() < owner->second->GetOrder()) {
                        if (assumed && assumed->first == branch.condition.first) {
                            const bool take_body = assumed->second == branch.condition.second;
                            return Bit(inst->Arg(take_body ? 1 - bypass : bypass), bit, depth - 1,
                                       assumed);
                        }
                        auto false_guard = branch.condition;
                        false_guard.second = !false_guard.second;
                        const auto no = Bit(inst->Arg(bypass), bit, depth - 1, false_guard);
                        const auto yes =
                            Bit(inst->Arg(1 - bypass), bit, depth - 1, branch.condition);
                        if (no && yes) {
                            if (*no == *yes)
                                return no;
                            const auto nc = Constant(*no);
                            const auto yc = Constant(*yes);
                            if (nc && yc) {
                                if (*nc == *yc)
                                    return Predicate{IR::Value{*yc}, true};
                                auto result = branch.condition;
                                result.second = result.second == *yc;
                                return result;
                            }
                        }
                    }
                }
            }
            return std::nullopt;
        }
        if (inst->GetOpcode() == IR::Opcode::SelectU32) {
            const auto yes = Bit(inst->Arg(1), bit, depth - 1, assumed);
            const auto no = Bit(inst->Arg(2), bit, depth - 1, assumed);
            if (yes && no) {
                if (*yes == *no)
                    return yes;
                const auto yc = Constant(*yes);
                const auto nc = Constant(*no);
                if (yc && nc) {
                    if (*nc == *yc)
                        return Predicate{IR::Value{*yc}, true};
                    auto cond = Literal(inst->Arg(0), depth - 1);
                    cond.second = cond.second == *yc;
                    return cond;
                }
            }
        }
        if (inst->GetOpcode() == IR::Opcode::BitwiseOr32 ||
            inst->GetOpcode() == IR::Opcode::BitwiseAnd32) {
            const auto lhs = Bit(inst->Arg(0), bit, depth - 1, assumed);
            const auto rhs = Bit(inst->Arg(1), bit, depth - 1, assumed);
            const bool is_or = inst->GetOpcode() == IR::Opcode::BitwiseOr32;
            if (lhs) {
                if (const auto c = Constant(*lhs))
                    return *c == is_or ? lhs : rhs;
            }
            if (rhs) {
                if (const auto c = Constant(*rhs))
                    return *c == is_or ? rhs : lhs;
            }
        }
        if (inst->GetOpcode() == IR::Opcode::BitFieldUExtract && inst->Arg(1).IsImmediate() &&
            inst->Arg(2).IsImmediate()) {
            if (bit >= inst->Arg(2).U32())
                return Predicate{IR::Value{false}, true};
            return Bit(inst->Arg(0), bit + inst->Arg(1).U32(), depth - 1, assumed);
        }
        return std::nullopt;
    }

    Predicate Literal(IR::Value value, u32 depth = 256) const {
        value = value.Resolve();
        if (value.IsImmediate() || depth == 0) {
            return {value, true};
        }
        const auto* inst = value.InstRecursive();
        switch (inst->GetOpcode()) {
        case IR::Opcode::ConditionRef:
            return Literal(inst->Arg(0), depth - 1);
        case IR::Opcode::LogicalNot: {
            auto result = Literal(inst->Arg(0), depth - 1);
            result.second = !result.second;
            return result;
        }
        case IR::Opcode::SelectU1: {
            if (const auto c = Constant(Literal(inst->Arg(0), depth - 1))) {
                return Literal(inst->Arg(*c ? 1 : 2), depth - 1);
            }
            break;
        }
        case IR::Opcode::IEqual:
        case IR::Opcode::INotEqual:
            if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() != 0) {
                break;
            }
            [[fallthrough]];
        case IR::Opcode::GetZeroFromOp: {
            const bool is_zero = inst->GetOpcode() != IR::Opcode::INotEqual;
            const auto input = inst->Arg(0).Resolve();
            if (input.IsImmediate() && input.Type() == IR::Type::U32) {
                return {IR::Value{input.U32() == 0}, is_zero};
            }
            if (!input.IsImmediate()) {
                const auto* extract = input.InstRecursive();
                if (extract->GetOpcode() == IR::Opcode::BitwiseAnd32) {
                    for (size_t arg = 0; arg < 2; ++arg) {
                        const auto mask = extract->Arg(arg).Resolve();
                        if (mask.IsImmediate() && std::has_single_bit(mask.U32())) {
                            if (auto result = Bit(extract->Arg(1 - arg),
                                                  std::countr_zero(mask.U32()), depth - 1)) {
                                result->second = result->second != is_zero;
                                return *result;
                            }
                        }
                    }
                }
                if (extract->GetOpcode() == IR::Opcode::BitFieldUExtract &&
                    extract->Arg(2).IsImmediate() && extract->Arg(2).U32() == 1) {
                    if (auto result = Bit(input, 0, depth - 1)) {
                        result->second = result->second != is_zero;
                        return *result;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        return {IR::Value{const_cast<IR::Inst*>(inst)}, true};
    }

    bool IsBackEdge(const IR::Inst* phi, size_t arg) const {
        const auto owner = owners.find(phi);
        return owner != owners.end() && phi->PhiBlock(arg)->GetOrder() >= owner->second->GetOrder();
    }

    bool AllowsPhi(const IR::Inst* phi, size_t arg) const {
        const auto owner = owners.find(phi);
        if (owner == owners.end() || !loops.contains(owner->second) || !loops.contains(current) ||
            loops.at(owner->second) != loops.at(current)) {
            return true;
        }
        const auto merge = merges.find(owner->second);
        const auto current_scopes = scopes.find(current);
        if (merge == merges.end() || current_scopes == scopes.end()) {
            return true;
        }
        for (const auto& branch : merge->second) {
            const auto [condition, positive] = branch.condition;
            for (const auto& known : current_scopes->second) {
                const auto [known_condition, known_positive] = known.condition;
                if (condition != known_condition) {
                    continue;
                }
                const bool taken = positive == known_positive;
                const bool bypass = phi->PhiBlock(arg) == branch.header;
                if (bypass == taken) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// Visits the constant buffer words summed into a value, nearest first, looking through phis
template <typename Func>
void ForEachSummedWord(const IR::Value& value, Func&& func,
                       const TrackingConditions* conditions = nullptr) {
    std::deque<std::tuple<IR::Value, u32, bool>> queue{{value, 0, true}};
    std::array<std::unordered_set<const IR::Inst*>, 2> visited;
    while (!queue.empty()) {
        const auto [term, depth, same_iteration] = queue.front();
        queue.pop_front();
        if (term.IsImmediate()) {
            continue;
        }
        const IR::Inst* const inst{term.InstRecursive()};
        if (!visited[same_iteration].insert(inst).second) {
            continue;
        }
        if (const auto word{CbufWord(inst)}) {
            func(*word, depth);
            continue;
        }
        if (inst->GetOpcode() == IR::Opcode::IAdd32 || inst->GetOpcode() == IR::Opcode::Phi ||
            inst->GetOpcode() == IR::Opcode::SelectU32) {
            for (size_t arg = inst->GetOpcode() == IR::Opcode::SelectU32 ? 1 : 0;
                 arg < inst->NumArgs(); ++arg) {
                if (inst->GetOpcode() == IR::Opcode::Phi && conditions && same_iteration &&
                    !conditions->AllowsPhi(inst, arg)) {
                    continue;
                }
                const bool next_iteration =
                    same_iteration && !(conditions && inst->GetOpcode() == IR::Opcode::Phi &&
                                        conditions->IsBackEdge(inst, arg));
                queue.emplace_back(inst->Arg(arg), depth + 1, next_iteration);
            }
        }
    }
}

struct WordSum {
    StorageBufferSet words;
    u32 count{};
    u32 budget{64};
    bool reaches_loop{};
};

/// Collects the constant buffer words of an addition tree and returns false when another value
/// feeds it. Reaching the phi given as the loop variable is recorded in the sum.
bool SumWords(const IR::Value& value, const IR::Inst* loop, WordSum& sum) {
    if (value.IsImmediate()) {
        return true;
    }
    if (sum.budget == 0) {
        return false;
    }
    --sum.budget;
    const IR::Inst* const inst{value.InstRecursive()};
    if (inst == loop) {
        sum.reaches_loop = true;
        return false;
    }
    if (const auto word{CbufWord(inst)}) {
        sum.words.insert(*word);
        ++sum.count;
        return true;
    }
    if (inst->GetOpcode() != IR::Opcode::IAdd32) {
        return false;
    }
    const bool lhs{SumWords(inst->Arg(0), loop, sum)};
    if (!lhs && !loop) {
        return false;
    }
    const bool rhs{SumWords(inst->Arg(1), loop, sum)};
    return lhs && rhs;
}

/// Finds the constant buffer words that step strided iterators (CUTLASS-style kernel parameters):
/// words added to a loop-carried value on its back edge, and words summed with two or more other
/// words into an offset table. A base pointer is neither.
StorageBufferSet CollectIteratorWords(IR::Program& program) {
    StorageBufferSet result;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Phi) {
                for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
                    WordSum sum;
                    SumWords(inst.Arg(arg), &inst, sum);
                    if (sum.reaches_loop) {
                        result.insert(sum.words.begin(), sum.words.end());
                    }
                }
            } else if (inst.GetOpcode() == IR::Opcode::IAdd32) {
                WordSum sum;
                if (SumWords(IR::Value{&inst}, nullptr, sum) && sum.words.size() >= 3) {
                    result.insert(sum.words.begin(), sum.words.end());
                }
            }
        }
    }
    std::unordered_map<const IR::Inst*, u32> indices;
    std::unordered_map<const IR::Inst*, u32> lowlinks;
    std::unordered_set<const IR::Inst*> on_stack;
    std::vector<const IR::Inst*> stack;
    u32 next_index{};
    const auto visit = [&](auto&& self, const IR::Inst* inst) -> void {
        if (indices.contains(inst)) {
            return;
        }
        const u32 index = next_index++;
        indices[inst] = lowlinks[inst] = index;
        stack.push_back(inst);
        on_stack.insert(inst);
        const auto op = inst->GetOpcode();
        if (op == IR::Opcode::IAdd32 || op == IR::Opcode::Phi || op == IR::Opcode::SelectU32) {
            for (size_t arg = op == IR::Opcode::SelectU32 ? 1 : 0; arg < inst->NumArgs(); ++arg) {
                if (inst->Arg(arg).IsImmediate()) {
                    continue;
                }
                const IR::Inst* next = inst->Arg(arg).InstRecursive();
                if (!indices.contains(next)) {
                    self(self, next);
                    lowlinks[inst] = std::min(lowlinks[inst], lowlinks[next]);
                } else if (on_stack.contains(next)) {
                    lowlinks[inst] = std::min(lowlinks[inst], indices[next]);
                }
            }
        }
        if (lowlinks[inst] != index) {
            return;
        }
        std::vector<const IR::Inst*> component;
        const IR::Inst* member;
        do {
            member = stack.back();
            stack.pop_back();
            on_stack.erase(member);
            component.push_back(member);
        } while (member != inst);
        if (component.size() < 2) {
            return;
        }
        const std::unordered_set<const IR::Inst*> members(component.begin(), component.end());
        for (const IR::Inst* add : component) {
            if (add->GetOpcode() != IR::Opcode::IAdd32) {
                continue;
            }
            for (size_t arg = 0; arg < add->NumArgs(); ++arg) {
                const auto& term = add->Arg(arg);
                if (!term.IsImmediate() && members.contains(term.InstRecursive())) {
                    continue;
                }
                ForEachSummedWord(term, [&](StorageBufferAddr word, u32) { result.insert(word); });
            }
        }
    };
    for (IR::Block* block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Phi) {
                visit(visit, &inst);
            }
        }
    }
    return result;
}

/// Returns true when a condition reads a constant buffer word, as the carry out of an addition to
/// that word does
bool ReadsCbufWord(const IR::Value& condition, StorageBufferAddr word) {
    std::deque<IR::Value> queue{condition};
    std::unordered_set<const IR::Inst*> visited;
    while (!queue.empty() && visited.size() < 64) {
        const IR::Value value{queue.front()};
        queue.pop_front();
        if (value.IsImmediate()) {
            continue;
        }
        const IR::Inst* const inst{value.InstRecursive()};
        if (!visited.insert(inst).second) {
            continue;
        }
        if (CbufWord(inst) == word) {
            return true;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            queue.push_back(inst->Arg(arg));
        }
    }
    return false;
}

/// Finds the 64-bit constant buffer pointers a global atomic's address is based on: the whole
/// address apart from an immediate offset, or a 64-bit sum whose high half adds the pointer's high
/// word and the carry out of its low word. An atomic has no global fallback, so such a pointer is a
/// buffer even while it is unmapped. A 32-bit scalar added to a pointer carries nothing.
StorageBufferSet CollectAtomicPointers(IR::Program& program) {
    const auto word = [](const IR::U32& value) {
        return value.IsImmediate() ? std::nullopt : CbufWord(value.InstRecursive());
    };
    StorageBufferSet result;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsGlobalMemoryWrite(inst) || inst.Type() == IR::Type::Void) {
                continue;
            }
            const std::optional<LowAddrInfo> addr{TrackLowAddress(&inst)};
            if (!addr) {
                continue;
            }
            const std::optional<StorageBufferAddr> low{word(addr->value)};
            if (low && Common::IsAligned(low->offset, 8) &&
                word(addr->high) ==
                    StorageBufferAddr{.index = low->index, .offset = low->offset + 4}) {
                result.insert(*low);
                continue;
            }
            StorageBufferSet high_words;
            small_vector<IR::Value, 2> carries;
            std::vector<IR::Value> pending{addr->high};
            std::unordered_set<const IR::Inst*> visited;
            while (!pending.empty()) {
                const IR::Value term{pending.back()};
                pending.pop_back();
                if (term.IsImmediate() || !visited.insert(term.InstRecursive()).second) {
                    continue;
                }
                const IR::Inst* const term_inst{term.InstRecursive()};
                if (const auto high_word{CbufWord(term_inst)}) {
                    high_words.insert(*high_word);
                    continue;
                }
                switch (term_inst->GetOpcode()) {
                case IR::Opcode::IAdd32:
                case IR::Opcode::Phi:
                    for (size_t arg = 0; arg < term_inst->NumArgs(); ++arg) {
                        pending.push_back(term_inst->Arg(arg));
                    }
                    break;
                case IR::Opcode::SelectU32:
                    if (term_inst->Arg(1).IsImmediate() && term_inst->Arg(2).IsImmediate()) {
                        carries.push_back(term_inst->Arg(0));
                    }
                    break;
                default:
                    break;
                }
            }
            if (carries.empty()) {
                continue;
            }
            ForEachSummedWord(addr->value, [&](StorageBufferAddr pointer, u32) {
                if (Common::IsAligned(pointer.offset, 8) &&
                    high_words.contains({.index = pointer.index, .offset = pointer.offset + 4}) &&
                    std::ranges::any_of(carries, [&](const IR::Value& carry) {
                        return ReadsCbufWord(carry, pointer);
                    })) {
                    result.insert(pointer);
                }
            });
        }
    }
    return result;
}

/// Finds the 64-bit constant buffer pointer summed into an address: its low word is summed into
/// the low half and its high word into the high half. Iterator steps and 32-bit scalars such as
/// strides and sizes are skipped. Competing bases require the legacy nearest-word fallback.
std::optional<StorageBufferAddr> TrackPointerAddend(const LowAddrInfo& addr,
                                                    const StorageBufferSet& iterator_words,
                                                    const TrackingConditions& conditions,
                                                    bool legacy_nearest = false) {
    StorageBufferSet high_words;
    ForEachSummedWord(
        addr.high, [&](StorageBufferAddr word, u32) { high_words.insert(word); }, &conditions);
    std::optional<StorageBufferAddr> result;
    u32 result_depth{};
    bool tied{};
    StorageBufferSet candidates;
    ForEachSummedWord(
        addr.value,
        [&](StorageBufferAddr word, u32 depth) {
            if (!Common::IsAligned(word.offset, 8) || iterator_words.contains(word) ||
                !high_words.contains({.index = word.index, .offset = word.offset + 4})) {
                return;
            }
            candidates.insert(word);
            if (!result || depth < result_depth) {
                result = word;
                result_depth = depth;
                tied = false;
            } else if (depth == result_depth && *result != word) {
                tied = true;
            }
        },
        &conditions);
    return tied || (!legacy_nearest && candidates.size() != 1) ? std::nullopt : result;
}

/// The first slot and stride of a pointer array read at a dynamic constant buffer offset:
/// (index << shift) + base or index * stride + base.
std::optional<std::pair<u32, u32>> DynamicPointerSlots(const IR::Value& offset) {
    const auto stride_of = [](const IR::Value& value) -> std::optional<u32> {
        if (value.IsImmediate()) {
            return std::nullopt;
        }
        const IR::Inst* const inst{value.InstRecursive()};
        if (inst->GetOpcode() == IR::Opcode::ShiftLeftLogical32 && inst->Arg(1).IsImmediate() &&
            inst->Arg(1).U32() < 16) {
            return 1U << inst->Arg(1).U32();
        }
        if (inst->GetOpcode() == IR::Opcode::IMul32 && inst->Arg(1).IsImmediate()) {
            return inst->Arg(1).U32();
        }
        return std::nullopt;
    };
    u32 base{};
    std::optional<u32> stride{stride_of(offset)};
    if (!stride && !offset.IsImmediate() &&
        offset.InstRecursive()->GetOpcode() == IR::Opcode::IAdd32) {
        const IR::Inst* const add{offset.InstRecursive()};
        for (size_t arg = 0; arg < 2 && !stride; ++arg) {
            if (add->Arg(arg ^ 1).IsImmediate()) {
                base = add->Arg(arg ^ 1).U32();
                stride = stride_of(add->Arg(arg));
            }
        }
    }
    if (!stride || *stride < 8 || !Common::IsAligned(*stride, 8)) {
        return std::nullopt;
    }
    return std::pair{base, *stride};
}

/// Slots of a pointer array read at a dynamic offset, following phis and selects of the offset.
/// A stride of zero is a single slot.
void CollectPointerSlots(const IR::Value& offset, small_vector<std::pair<u32, u32>, 4>& slots,
                         std::unordered_set<const IR::Inst*>& visited) {
    if (offset.IsImmediate()) {
        if (std::ranges::find(slots, std::pair{offset.U32(), 0U}) == slots.end()) {
            slots.emplace_back(offset.U32(), 0U);
        }
        return;
    }
    const IR::Inst* const inst{offset.InstRecursive()};
    if (!visited.insert(inst).second) {
        return;
    }
    if (const auto pattern{DynamicPointerSlots(offset)}) {
        if (std::ranges::find(slots, *pattern) == slots.end()) {
            slots.push_back(*pattern);
        }
        return;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::Phi:
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            CollectPointerSlots(inst->Arg(arg), slots, visited);
        }
        break;
    case IR::Opcode::SelectU32:
        CollectPointerSlots(inst->Arg(1), slots, visited);
        CollectPointerSlots(inst->Arg(2), slots, visited);
        break;
    default:
        break;
    }
}

/// Native programs: every constant buffer pointer the low address can come from, following only
/// additions, selects and phis. A predicated address assignment becomes a phi whose other input
/// can point into a different buffer, so the search does not stop at the first pointer. A pointer
/// read from an array at a dynamic offset contributes every mapped slot of the array.
small_vector<StorageBufferAddr, 4> NativePointerCandidates(const IR::U32& low_addr,
                                                           Environment& env, bool& dynamic,
                                                           StorageInfo& info) {
    small_vector<StorageBufferAddr, 4> candidates;
    small_vector<const IR::Inst*, 16> pending{low_addr.InstRecursive()};
    std::unordered_set<const IR::Inst*> visited{pending.front()};
    const auto enqueue = [&](const IR::Value& value) {
        if (!value.IsImmediate() && visited.insert(value.InstRecursive()).second) {
            pending.push_back(value.InstRecursive());
        }
    };
    for (size_t next = 0; next < pending.size(); ++next) {
        const IR::Inst* const inst{pending[next]};
        switch (inst->GetOpcode()) {
        case IR::Opcode::GetCbufU32:
        case IR::Opcode::GetCbufU32x2: {
            const IR::Value index{inst->Arg(0)};
            const IR::Value offset{inst->Arg(1)};
            if (index.IsImmediate() && !offset.IsImmediate()) {
                static constexpr u32 max_slots{16};
                small_vector<std::pair<u32, u32>, 4> slots;
                std::unordered_set<const IR::Inst*> slot_visited;
                CollectPointerSlots(offset, slots, slot_visited);
                for (const auto& [first, stride] : slots) {
                    for (u32 slot = 0; slot < (stride ? max_slots : 1U); ++slot) {
                        const StorageBufferAddr pointer{.index = index.U32(),
                                                        .offset = first + slot * stride};
                        if (Common::IsAligned(pointer.offset, 8) &&
                            std::ranges::find(candidates, pointer) == candidates.end() &&
                            env.IsCbufStorageBufferPointer(pointer.index, pointer.offset)) {
                            candidates.push_back(pointer);
                            dynamic = true;
                        }
                    }
                }
                break;
            }
            if (!index.IsImmediate() || !offset.IsImmediate()) {
                break;
            }
            const StorageBufferAddr pointer{.index = index.U32(), .offset = offset.U32()};
            if (Common::IsAligned(pointer.offset, 8) &&
                std::ranges::find(candidates, pointer) == candidates.end() &&
                env.IsCbufStorageBufferPointer(pointer.index, pointer.offset)) {
                candidates.push_back(pointer);
            }
            break;
        }
        case IR::Opcode::IAdd32:
        case IR::Opcode::Phi:
            for (size_t arg = inst->NumArgs(); arg--;) {
                enqueue(inst->Arg(arg));
            }
            break;
        case IR::Opcode::SelectU32:
            enqueue(inst->Arg(1));
            enqueue(inst->Arg(2));
            break;
        case IR::Opcode::ShuffleIndex:
            // A pointer made uniform (R2UR broadcasts the first active lane's value).
            enqueue(inst->Arg(0));
            break;
        case IR::Opcode::CompositeExtractU32x2:
            if (inst->Arg(1).IsImmediate() && inst->Arg(1).U32() == 0) {
                enqueue(inst->Arg(0));
            }
            break;
        case IR::Opcode::LoadGlobal64: {
            // A pointer loaded from the memory a constant buffer pointer points to (the binding
            // reads it again for every dispatch and draw).
            // The table address: a tracked low word, or a constant buffer pointer packed whole.
            const IR::Inst* holder_root{};
            s32 holder_offset{};
            const auto holder_addr{TrackLowAddress(const_cast<IR::Inst*>(inst))};
            if (holder_addr) {
                if (holder_addr->value.IsImmediate()) {
                    break;
                }
                holder_root = holder_addr->value.InstRecursive();
                holder_offset = holder_addr->imm_offset;
            } else {
                IR::Value address{inst->Arg(0)};
                if (!address.IsImmediate() &&
                    address.InstRecursive()->GetOpcode() == IR::Opcode::IAdd64 &&
                    address.InstRecursive()->Arg(1).IsImmediate()) {
                    holder_offset =
                        static_cast<s32>(static_cast<s64>(address.InstRecursive()->Arg(1).U64()));
                    address = address.InstRecursive()->Arg(0);
                }
                if (address.IsImmediate() ||
                    address.InstRecursive()->GetOpcode() != IR::Opcode::PackUint2x32 ||
                    address.InstRecursive()->Arg(0).IsImmediate()) {
                    break;
                }
                holder_root = address.InstRecursive()->Arg(0).InstRecursive();
            }
            if (holder_offset < 0 || !Common::IsAligned(holder_offset, 8)) {
                break;
            }
            const u32 indirect{static_cast<u32>(holder_offset)};
            // The table pointer can also be one of several constant buffer pointers (a phi or
            // select of them), or an entry of a constant buffer array indexed at run time (one
            // table per object type: every slot whose table entry is a pointer). Those are table
            // candidates, bound only in the bindings the other storage buffers leave free.
            const auto add = [&](const StorageBufferAddr& pointer, bool table) {
                if (std::ranges::find(candidates, pointer) != candidates.end()) {
                    return;
                }
                if (table && env.ShaderStage() != Stage::Compute) {
                    // Graphics programs bind every candidate for every draw: table entries
                    // multiply the storage buffers of draws that never read them.
                    return;
                }
                if (table) {
                    info.tables_seen = true;
                    if (!info.tables_enabled) {
                        return;
                    }
                    if (std::ranges::find(info.table_candidates, pointer) ==
                        info.table_candidates.end()) {
                        info.table_candidates.push_back(pointer);
                    }
                }
                candidates.push_back(pointer);
                dynamic = true;
            };
            small_vector<std::pair<const IR::Inst*, bool>, 4> holders{{holder_root, false}};
            std::unordered_set<const IR::Inst*> holder_visited{holder_root};
            for (size_t pending_holder = 0; pending_holder < holders.size(); ++pending_holder) {
                const auto [holder, via_phi] = holders[pending_holder];
                const auto enqueue_holder = [&](const IR::Value& value, bool branch) {
                    if (!value.IsImmediate() && holders.size() < 8 &&
                        holder_visited.insert(value.InstRecursive()).second) {
                        holders.emplace_back(value.InstRecursive(), via_phi || branch);
                    }
                };
                switch (holder->GetOpcode()) {
                case IR::Opcode::CompositeExtractU32x2:
                    if (holder->Arg(1).IsImmediate() && holder->Arg(1).U32() == 0) {
                        enqueue_holder(holder->Arg(0), false);
                    }
                    continue;
                case IR::Opcode::Phi:
                    for (size_t arg = 0; arg < holder->NumArgs(); ++arg) {
                        enqueue_holder(holder->Arg(arg), true);
                    }
                    continue;
                case IR::Opcode::SelectU32:
                    enqueue_holder(holder->Arg(1), true);
                    enqueue_holder(holder->Arg(2), true);
                    continue;
                case IR::Opcode::GetCbufU32:
                case IR::Opcode::GetCbufU32x2:
                    break;
                default:
                    continue;
                }
                if (!holder->Arg(0).IsImmediate()) {
                    continue;
                }
                const u32 index{holder->Arg(0).U32()};
                if (holder->Arg(1).IsImmediate()) {
                    const StorageBufferAddr pointer{
                        .index = index, .offset = holder->Arg(1).U32(), .indirect = indirect};
                    if (Common::IsAligned(pointer.offset, 8) &&
                        env.IsCbufStorageBufferPointer(pointer.index, pointer.offset)) {
                        add(pointer, via_phi || !holder_addr);
                    }
                    continue;
                }
                static constexpr u32 max_slots{16};
                small_vector<std::pair<u32, u32>, 4> slots;
                std::unordered_set<const IR::Inst*> slot_visited;
                CollectPointerSlots(holder->Arg(1), slots, slot_visited);
                for (const auto& [first, stride] : slots) {
                    for (u32 slot = 0; slot < (stride ? max_slots : 1U); ++slot) {
                        const StorageBufferAddr pointer{
                            .index = index, .offset = first + slot * stride, .indirect = indirect};
                        if (Common::IsAligned(pointer.offset, 8) &&
                            env.IsCbufIndirectStorageBufferPointer(pointer.index, pointer.offset,
                                                                   pointer.indirect)) {
                            add(pointer, true);
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return candidates;
}

/// Collects the storage buffer used by a global memory instruction and the instruction itself
void CollectStorageBuffers(IR::Block& block, IR::Inst& inst, StorageInfo& info, Environment& env,
                           const TrackingConditions& conditions) {
    // NVN puts storage buffers in a specific range, we have to bias towards these addresses to
    // avoid getting false positives
    static constexpr Bias nvn_bias{
        .index = 0,
        .offset_begin = 0x100,
        .offset_end = 0x700,
        .alignment = 16,
    };
    // Track the low address of the instruction
    const std::optional<LowAddrInfo> low_addr_info{TrackLowAddress(&inst)};
    if (!low_addr_info) {
        // Failed to track the low address, use NVN fallbacks
        return;
    }
    // First try to find storage buffers in the NVN address
    const IR::U32 low_addr{low_addr_info->value};
    const std::optional<StorageBufferAddr> paired_pointer{
        TrackPointerAddend(*low_addr_info, info.iterator_words, conditions)};
    std::optional<StorageBufferAddr> storage_buffer{paired_pointer};
    const bool native = env.IsNativeProgram();
    if (native && storage_buffer &&
        !env.IsCbufStorageBufferPointer(storage_buffer->index, storage_buffer->offset)) {
        storage_buffer.reset();
    }
    // A paired constant buffer pointer can be only one input of a select or phi whose other input
    // is a pointer loaded from that buffer (Ocarina of Time's foliage unit placement records are
    // read from c0[12] or from *c0[12]). Rebasing on the constant buffer pointer then reads the
    // loaded path's records out of bounds, as zeros, so look for loaded pointers here as well.
    const bool paired_found = storage_buffer.has_value();
    if (native && !low_addr.IsImmediate()) {
        const IR::Inst* const start{low_addr.InstRecursive()};
        auto it{info.native_candidates.find(start)};
        if (it == info.native_candidates.end()) {
            bool dynamic{};
            it = info.native_candidates
                     .emplace(start, NativePointerCandidates(low_addr, env, dynamic, info))
                     .first;
            if (dynamic) {
                info.native_dynamic.insert(start);
            }
        }
        const auto& candidates{it->second};
        const bool has_loaded_pointer{std::ranges::any_of(
            candidates, [](const StorageBufferAddr& c) { return c.indirect != ~0U; })};
        if (paired_found && !has_loaded_pointer) {
            // No pointer loaded from memory competes with the paired one: keep the pair.
        } else if (candidates.size() > 1 || paired_found ||
                   (!candidates.empty() && info.native_dynamic.contains(start))) {
            // Bind every candidate and keep the access global; the backend picks the buffer at
            // run time. Rebasing on one of them drops the other path's accesses.
            bool accepted{};
            if (paired_found) {
                // The paired pointer is the other input of the select or phi; bind it too.
                info.set.insert(*storage_buffer);
                if (IsGlobalMemoryWrite(inst)) {
                    info.writes.insert(*storage_buffer);
                }
            }
            for (const StorageBufferAddr& candidate : candidates) {
                if (std::ranges::find(info.table_candidates, candidate) !=
                    info.table_candidates.end()) {
                    const StorageBufferAddr holder{.index = candidate.index,
                                                   .offset = candidate.offset};
                    const size_t added{
                        (info.set.find(candidate) == info.set.end() ? 1U : 0U) +
                        (candidate.indirect != ~0U && info.set.find(holder) == info.set.end()
                             ? 1U
                             : 0U)};
                    if (info.table_used + added > info.table_budget) {
                        continue;
                    }
                    info.table_used += added;
                }
                info.set.insert(candidate);
                if (IsGlobalMemoryWrite(inst)) {
                    info.writes.insert(candidate);
                }
                if (candidate.indirect != ~0U) {
                    // The backend reads the pointer through the buffer that holds it.
                    info.set.insert(StorageBufferAddr{.index = candidate.index,
                                                      .offset = candidate.offset});
                }
                accepted = true;
            }
            if (accepted) {
                info.native_runtime_bounds = true;
                return;
            }
            // Only table entries, and no free binding for them: as if none had been found.
        } else if (candidates.size() == 1 && !paired_found) {
            // The one pointer the address is built from. The NVN search below only takes 16-byte
            // aligned slots, so for a pointer at an 8-byte slot it can return a dimension or stride
            // the address arithmetic reads instead, and the access then lands in the wrong buffer.
            storage_buffer = candidates.front();
        }
    }
    if (native && !storage_buffer) {
        // A pointer that is an atomic's address keeps its buffer while unmapped, for the atomic
        // and every access based on it: Bananza's append counters and the records stored after
        // them were unmapped when their pipelines were first built. The binding reads the pointer
        // again for every dispatch.
        if (!paired_pointer || !info.atomic_pointers.contains(*paired_pointer)) {
            return;
        }
        storage_buffer = paired_pointer;
    }
    const auto cached_track = [&](const Bias* bias, auto& cache) {
        if (low_addr.IsImmediate()) {
            return Track(low_addr, bias);
        }
        const IR::Inst* const start{low_addr.InstRecursive()};
        if (const auto it = cache.find(start); it != cache.end()) {
            return it->second;
        }
        const std::optional<StorageBufferAddr> result{Track(low_addr, bias)};
        cache.emplace(start, result);
        return result;
    };
    if (!storage_buffer) {
        storage_buffer = TrackPointerAddend(*low_addr_info, info.iterator_words, conditions, true);
    }
    if (!storage_buffer) {
        storage_buffer = TrackAddend(low_addr);
    }
    if (!storage_buffer) {
        storage_buffer = cached_track(&nvn_bias, info.biased_tracks);
    }
    if (!storage_buffer) {
        // If it fails, track without a bias
        storage_buffer = cached_track(nullptr, info.unbiased_tracks);
        if (!storage_buffer) {
            // If that also fails, use NVN fallbacks
            LOG_WARNING(Shader, "Storage buffer failed to track, using global memory fallbacks");
            return;
        }
        LOG_DEBUG(Shader, "Storage buffer tracked without bias, index {} offset {}",
                    storage_buffer->index, storage_buffer->offset);
    }
    // Collect storage buffer and the instruction
    if (IsGlobalMemoryWrite(inst)) {
        info.writes.insert(*storage_buffer);
    }
    info.set.insert(*storage_buffer);
    info.to_replace.push_back(StorageInst{
        .storage_buffer{*storage_buffer},
        .inst = &inst,
        .block = &block,
    });
}

/// Returns the offset in indices (not bytes) for an equivalent storage instruction
IR::U32 StorageOffset(IR::Block& block, IR::Inst& inst, StorageBufferAddr buffer, u32 alignment) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    IR::U32 offset;
    if (const std::optional<LowAddrInfo> low_addr{TrackLowAddress(&inst)}) {
        offset = low_addr->value;
        if (low_addr->imm_offset != 0) {
            offset = ir.IAdd(offset, ir.Imm32(low_addr->imm_offset));
        }
    } else {
        offset = ir.UConvert(32, IR::U64{inst.Arg(0)});
    }
    // Subtract the least significant 32 bits from the guest offset. The result is the storage
    // buffer offset in bytes.
    IR::U32 low_cbuf{ir.GetCbuf(ir.Imm32(buffer.index), ir.Imm32(buffer.offset))};

    // Align the offset base to match the host alignment requirements
    low_cbuf = ir.BitwiseAnd(low_cbuf, ir.Imm32(~(alignment - 1U)));
    return ir.ISub(offset, low_cbuf);
}

/// Replace a global memory load instruction with its storage buffer equivalent
void ReplaceLoad(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                 const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{
        &*block.PrependNewInst(it, new_opcode, {storage_index, offset}, inst.Flags<u32>())};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory write instruction with its storage buffer equivalent
void ReplaceWrite(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                  const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)});
    inst.Invalidate();
}

/// Replace an atomic operation on global memory instruction with its storage buffer equivalent
void ReplaceAtomic(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                   const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{
        &*block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory instruction with its storage buffer equivalent
void Replace(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
             const IR::U32& offset) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
        return ReplaceLoad(block, inst, storage_index, offset);
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
        return ReplaceWrite(block, inst, storage_index, offset);
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return ReplaceAtomic(block, inst, storage_index, offset);
    default:
        throw InvalidArgument("Invalid global memory opcode {}", inst.GetOpcode());
    }
}
} // Anonymous namespace

void GlobalMemoryToStorageBufferPass(Environment& env, IR::Program& program,
                                     const HostTranslateInfo& host_info) {
    const bool has_global_memory =
        std::ranges::any_of(program.post_order_blocks, [](const auto* block) {
            return std::ranges::any_of(block->Instructions(), IsGlobalMemory);
        });
    if (!has_global_memory) {
        return;
    }
    TrackingConditions conditions{program};
    StorageInfo info;
    info.iterator_words = CollectIteratorWords(program);
    info.atomic_pointers = CollectAtomicPointers(program);
    const auto collect = [&] {
        for (IR::Block* const block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (!IsGlobalMemory(inst)) {
                    continue;
                }
                conditions.current = block;
                CollectStorageBuffers(*block, inst, info, env, conditions);
            }
        }
    };
    collect();
    // Compute programs may bind up to Info::MAX_SSBOS (64) storage buffers, so pointer table
    // entries still fit when the program's own pointers take most of 32. Ocarina of Time's foliage
    // list builder has 29 of its own: the table entries of its last two sections (Hyrule Field
    // grass) did not fit, read zeros and the grass list stayed empty.
    // EDEN_NX2_WIDE_COMPUTE_SSBOS=0 restores the 32-binding limit.
    static const bool wide_compute = [] {
        const char* const value = std::getenv("EDEN_NX2_WIDE_COMPUTE_SSBOS");
        return !(value && value[0] == '0');
    }();
    const size_t max_ssbos{env.ShaderStage() == Stage::Compute && wide_compute
                               ? Info::MAX_SSBOS
                               : Info::MAX_GRAPHICS_SSBOS};
    if (info.tables_seen && info.set.size() < max_ssbos) {
        // Again with pointer table entries, in the bindings the first pass left free.
        StorageInfo with_tables;
        with_tables.iterator_words = std::move(info.iterator_words);
        with_tables.atomic_pointers = std::move(info.atomic_pointers);
        with_tables.tables_enabled = true;
        with_tables.table_budget = max_ssbos - info.set.size();
        info = std::move(with_tables);
        collect();
    }
    if (info.set.size() > max_ssbos) {
        throw NotImplementedException("{} storage buffers, the limit is {}", info.set.size(),
                                      max_ssbos);
    }
    program.info.native_global_bounds |= info.native_runtime_bounds;
    for (const StorageBufferAddr& storage_buffer : info.set) {
        program.info.storage_buffers_descriptors.push_back({
            .cbuf_index = storage_buffer.index,
            .cbuf_offset = storage_buffer.offset,
            .count = 1,
            .is_written = info.writes.contains(storage_buffer),
            .indirect_offset = storage_buffer.indirect,
        });
    }
    for (const StorageInst& storage_inst : info.to_replace) {
        const StorageBufferAddr storage_buffer{storage_inst.storage_buffer};
        const auto it{info.set.find(storage_inst.storage_buffer)};
        const IR::U32 index{IR::Value{static_cast<u32>(info.set.index_of(it))}};
        IR::Block* const block{storage_inst.block};
        IR::Inst* const inst{storage_inst.inst};
        const IR::U32 offset{
            StorageOffset(*block, *inst, storage_buffer, u32(host_info.min_ssbo_alignment))};
        Replace(*block, *inst, index, offset);
    }
}

void JoinStorageInfo(Info& base, Info& source) {
    base.native_global_bounds |= source.native_global_bounds;
    auto& descriptors = base.storage_buffers_descriptors;
    for (auto& desc : source.storage_buffers_descriptors) {
        auto it{std::ranges::find_if(descriptors, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.indirect_offset == existing.indirect_offset;
        })};
        if (it != descriptors.end()) {
            it->is_written |= desc.is_written;
            continue;
        }
        descriptors.push_back(desc);
    }
}

} // namespace Shader::Optimization
