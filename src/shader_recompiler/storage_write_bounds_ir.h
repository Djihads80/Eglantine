// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <bit>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/storage_write_bounds.h"

namespace Shader::StorageWriteBounds {

namespace Detail {
// Avoid recursive identity resolution on malformed/cyclic input. Normal validated IR
// should never hit the bound, but this optional optimizer must fail closed.
inline std::optional<IR::Value> Resolve(IR::Value value) {
    for (u32 depth = 0; depth < 64; ++depth) {
        if (!value.IsIdentity()) {
            return value;
        }
        value = value.Inst()->Arg(0);
    }
    return std::nullopt;
}

inline std::optional<u32> Immediate(IR::Value input) {
    const auto value = Resolve(input);
    if (!value || !value->IsImmediate() || value->Type() != IR::Type::U32) {
        return std::nullopt;
    }
    return value->U32();
}

inline const IR::Inst* Instruction(IR::Value input) {
    const auto value = Resolve(input);
    return value && !value->IsImmediate() ? value->Inst() : nullptr;
}

inline bool BaseRead(IR::Value value, const DescriptorProof& proof) {
    const IR::Inst* inst = Instruction(value);
    if (!inst) {
        return false;
    }
    if (inst->GetOpcode() == IR::Opcode::CompositeExtractU32x2) {
        if (Immediate(inst->Arg(1)) != std::optional<u32>{0}) {
            return false;
        }
        inst = Instruction(inst->Arg(0));
        if (!inst || inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return false;
        }
    } else if (inst->GetOpcode() != IR::Opcode::GetCbufU32) {
        return false;
    }
    return Immediate(inst->Arg(0)) == std::optional<u32>{proof.cbuf_index} &&
           Immediate(inst->Arg(1)) == std::optional<u32>{proof.cbuf_offset};
}

// Algebraic form: (has_base ? B : 0) + offset (mod 2^32). Only one exact cbuf
// base is permitted. The generated ISub cancels B; Evaluate then proves that
// the remaining offset + alignment bias does NOT wrap, including intermediates.
struct Expression {
    u32 node{};
    bool has_base{};
};

class Extractor {
public:
    explicit Extractor(DescriptorProof& proof_) : proof{proof_} {}

    std::optional<u32> Offset(IR::Value value, u32 alignment) {
        const IR::Inst* subtract = Instruction(value);
        if (!subtract || subtract->GetOpcode() != IR::Opcode::ISub32) {
            return std::nullopt;
        }
        const IR::Inst* mask = Instruction(subtract->Arg(1));
        if (!mask || mask->GetOpcode() != IR::Opcode::BitwiseAnd32) {
            return std::nullopt;
        }
        const auto is_aligned_base = [&](IR::Value base, IR::Value bits) {
            return BaseRead(base, proof) &&
                   Immediate(bits) == std::optional<u32>{~(alignment - 1U)};
        };
        if (!is_aligned_base(mask->Arg(0), mask->Arg(1)) &&
            !is_aligned_base(mask->Arg(1), mask->Arg(0))) {
            return std::nullopt;
        }
        const auto address = Parse(subtract->Arg(0), 0);
        if (!address || !address->has_base) {
            return std::nullopt;
        }
        const auto bias = Add({Op::AlignmentBias, alignment});
        if (!bias) {
            return std::nullopt;
        }
        return Add({Op::AddU32, address->node, *bias});
    }

private:
    std::optional<u32> Add(Node node) {
        if (proof.nodes.size() >= 256) {
            return std::nullopt;
        }
        const u32 result = static_cast<u32>(proof.nodes.size());
        proof.nodes.push_back(node);
        return result;
    }

    std::optional<Expression> Leaf(Node node, bool base = false) {
        const auto id = Add(node);
        return id ? std::optional<Expression>{{*id, base}} : std::nullopt;
    }

    std::optional<Expression> Parse(IR::Value input, u32 depth) {
        if (depth >= 64) {
            return std::nullopt;
        }
        const auto value = Resolve(input);
        if (!value || value->Type() != IR::Type::U32) {
            return std::nullopt;
        }
        if (value->IsImmediate()) {
            return Leaf({Op::Constant, value->U32()});
        }
        const IR::Inst* inst = value->Inst();
        if (const auto it = memo.find(inst); it != memo.end()) {
            return it->second;
        }
        if (!active.insert(inst).second) {
            return std::nullopt;
        }
        const auto result = ParseInstruction(*inst, depth);
        active.erase(inst);
        memo.emplace(inst, result);
        return result;
    }

    std::optional<Expression> ParseInstruction(const IR::Inst& inst, u32 depth) {
        if (BaseRead(IR::Value{const_cast<IR::Inst*>(&inst)}, proof)) {
            return Leaf({Op::Constant, 0}, true);
        }
        if (inst.GetOpcode() == IR::Opcode::CompositeExtractU32x3) {
            const auto axis = Immediate(inst.Arg(1));
            const IR::Inst* vector = Instruction(inst.Arg(0));
            if (!axis || *axis >= 3 || !vector) {
                return std::nullopt;
            }
            if (vector->GetOpcode() == IR::Opcode::WorkgroupId) {
                return Leaf({Op::Workgroup, *axis});
            }
            if (vector->GetOpcode() == IR::Opcode::LocalInvocationId) {
                return Leaf({Op::LocalInvocation, *axis});
            }
            return std::nullopt;
        }
        if (inst.GetOpcode() == IR::Opcode::BitFieldUExtract) {
            const auto offset = Immediate(inst.Arg(1));
            const auto width = Immediate(inst.Arg(2));
            if (!offset || !width || *offset > 32 || *width > 32 - *offset) {
                return std::nullopt;
            }
            const auto input = Parse(inst.Arg(0), depth + 1);
            if (!input || input->has_base) {
                return std::nullopt;
            }
            const auto id = Add({Op::BitFieldExtractU32, input->node, (*offset << 8) | *width});
            return id ? std::optional<Expression>{{*id, false}} : std::nullopt;
        }
        Op operation{};
        std::size_t left_arg = 0;
        std::size_t right_arg = 1;
        switch (inst.GetOpcode()) {
        case IR::Opcode::IAdd32: operation = Op::AddU32; break;
        case IR::Opcode::IMul32: operation = Op::MulU32; break;
        case IR::Opcode::ShiftLeftLogical32: operation = Op::ShiftLeftU32; break;
        case IR::Opcode::SelectU32:
            operation = Op::SelectUnion;
            left_arg = 1;
            right_arg = 2;
            break;
        default:
            // Includes Phi, cbuf scalar/stride, SSBO reads, signed and modular ops.
            return std::nullopt;
        }
        const auto left = Parse(inst.Arg(left_arg), depth + 1);
        const auto right = Parse(inst.Arg(right_arg), depth + 1);
        if (!left || !right) {
            return std::nullopt;
        }
        bool has_base = false;
        if (operation == Op::AddU32) {
            if (left->has_base && right->has_base) {
                return std::nullopt;
            }
            has_base = left->has_base || right->has_base;
        } else if (operation == Op::SelectUnion) {
            if (left->has_base != right->has_base) {
                return std::nullopt;
            }
            has_base = left->has_base;
        } else if (left->has_base || right->has_base) {
            return std::nullopt;
        }
        const auto id = Add({operation, left->node, right->node});
        return id ? std::optional<Expression>{{*id, has_base}} : std::nullopt;
    }

    DescriptorProof& proof;
    std::unordered_map<const IR::Inst*, std::optional<Expression>> memo;
    std::unordered_set<const IR::Inst*> active;
};

inline u32 WriteWidth(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::WriteStorage32:
    case IR::Opcode::StorageAtomicIAdd32:
    case IR::Opcode::StorageAtomicSMin32:
    case IR::Opcode::StorageAtomicUMin32:
    case IR::Opcode::StorageAtomicSMax32:
    case IR::Opcode::StorageAtomicUMax32:
    case IR::Opcode::StorageAtomicInc32:
    case IR::Opcode::StorageAtomicDec32:
    case IR::Opcode::StorageAtomicAnd32:
    case IR::Opcode::StorageAtomicOr32:
    case IR::Opcode::StorageAtomicXor32:
    case IR::Opcode::StorageAtomicExchange32:
        return 4;
    case IR::Opcode::WriteStorage64:
    case IR::Opcode::StorageAtomicIAdd64:
    case IR::Opcode::StorageAtomicSMin64:
    case IR::Opcode::StorageAtomicUMin64:
    case IR::Opcode::StorageAtomicSMax64:
    case IR::Opcode::StorageAtomicUMax64:
    case IR::Opcode::StorageAtomicAnd64:
    case IR::Opcode::StorageAtomicOr64:
    case IR::Opcode::StorageAtomicXor64:
    case IR::Opcode::StorageAtomicExchange64:
    case IR::Opcode::StorageAtomicIAdd32x2:
    case IR::Opcode::StorageAtomicSMin32x2:
    case IR::Opcode::StorageAtomicUMin32x2:
    case IR::Opcode::StorageAtomicSMax32x2:
    case IR::Opcode::StorageAtomicUMax32x2:
    case IR::Opcode::StorageAtomicAnd32x2:
    case IR::Opcode::StorageAtomicOr32x2:
    case IR::Opcode::StorageAtomicXor32x2:
    case IR::Opcode::StorageAtomicExchange32x2:
        return 8;
    case IR::Opcode::WriteStorage128:
        return 16;
    default:
        return 0;
    }
}
// The optional global-read case has a stricter whole-program side-effect policy.
// Only these storage writes are accounted below. Control/barrier operations do not
// write buffers; all other side effects fail closed, including local/shared writes.
inline bool AllowedWithGlobalReads(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::WriteStorage32:
    case IR::Opcode::WriteStorage64:
    case IR::Opcode::WriteStorage128:
    case IR::Opcode::ConditionRef:
    case IR::Opcode::Reference:
    case IR::Opcode::PhiMove:
    case IR::Opcode::Prologue:
    case IR::Opcode::Epilogue:
    case IR::Opcode::Join:
    case IR::Opcode::Barrier:
    case IR::Opcode::WorkgroupMemoryBarrier:
    case IR::Opcode::DeviceMemoryBarrier:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    // These three backend operations only sample/query a texture. Their values
    // still cannot enter the accepted write-address DAG.
    case IR::Opcode::ImageFetch:
    case IR::Opcode::ImageQueryDimensions:
    case IR::Opcode::ImageSampleExplicitLod:
        return true;
    default:
        break;
    }
    const auto name = IR::NameOf(inst.GetOpcode());
    return !inst.MayHaveSideEffects() && !name.starts_with("Write") &&
           !name.starts_with("LoadGlobal") && name.find("Atomic") == std::string_view::npos &&
           name.find("Image") == std::string_view::npos;
}
} // namespace Detail

// Called on the final translated compute IR. Only the exact address form emitted
// by GlobalMemoryToStorageBufferPass is recognized; later transformations that
// change it fail closed. Scan all blocks/stores, never sampled predicates.
inline std::vector<DescriptorProof> ExtractMemory(const IR::Program& program, u32 alignment,
                                                  bool include_reads, bool allow_global_reads = false) {
    const auto& descriptors = program.info.storage_buffers_descriptors;
    std::vector<DescriptorProof> result(descriptors.size());
    const auto fail_all = [&]() {
        for (auto& proof : result) {
            proof.complete = false;
        }
        return result;
    };
    if (include_reads && (!program.info.image_descriptors.empty() ||
        !program.info.image_buffer_descriptors.empty() || !program.info.texture_descriptors.empty() ||
        !program.info.texture_buffer_descriptors.empty())) return result;
    const bool global_reads_only = program.info.uses_global_memory &&
                                   allow_global_reads && !include_reads;
    if (global_reads_only && (program.info.stores_global_memory ||
        !program.info.image_descriptors.empty() || !program.info.image_buffer_descriptors.empty())) {
        return result;
    }
    if (program.stage != Stage::Compute || !std::has_single_bit(alignment) ||
        alignment > 65536 || program.info.native_global_bounds ||
        (program.info.uses_global_memory && !global_reads_only) ||
        program.blocks.empty() || descriptors.size() > 32) {
        return result;
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        const auto& descriptor = descriptors[index];
        result[index].cbuf_index = descriptor.cbuf_index;
        result[index].cbuf_offset = descriptor.cbuf_offset;
        result[index].binding_alignment = alignment;
        result[index].complete = (include_reads || descriptor.is_written) && descriptor.count == 1;
    }
    // Reject loops even when the current address slice looks invariant. Initial
    // scope deliberately excludes loop/phi/control-flow induction reasoning.
    std::unordered_set<const IR::Block*> blocks(program.blocks.begin(), program.blocks.end());
    blocks.insert(program.post_order_blocks.begin(), program.post_order_blocks.end());
    for (const auto& node : program.syntax_list) {
        using Type = IR::AbstractSyntaxNode::Type;
        if (node.type == Type::Loop || node.type == Type::Repeat || node.type == Type::Break) {
            return fail_all();
        }
        if (node.type == Type::Block) {
            blocks.insert(node.data.block);
        }
    }
    if (blocks.size() > 1024 || blocks.contains(nullptr)) {
        return fail_all();
    }
    std::vector<Detail::Extractor> extractors;
    extractors.reserve(result.size());
    for (auto& proof : result) {
        extractors.emplace_back(proof);
    }
    std::size_t instruction_count = 0;
    for (const IR::Block* block : blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (++instruction_count > 262144) {
                return fail_all();
            }
            if (global_reads_only && !Detail::AllowedWithGlobalReads(inst)) {
                return fail_all();
            }
            const auto name = IR::NameOf(inst.GetOpcode());
            if (name.starts_with("WriteGlobal") || name.starts_with("GlobalAtomic") ||
                (include_reads && name.starts_with("LoadGlobal"))) {
                return fail_all();
            }
            if (!name.starts_with("WriteStorage") && !name.starts_with("StorageAtomic") &&
                !(include_reads && name.starts_with("LoadStorage"))) {
                continue;
            }
            const auto index = Detail::Immediate(inst.Arg(0));
            if (!index || *index >= result.size()) {
                return fail_all();
            }
            auto& proof = result[*index];
            if (!proof.complete) {
                continue;
            }
            const u32 width = [&] {
                if (!name.starts_with("LoadStorage")) return Detail::WriteWidth(inst.GetOpcode());
                switch (inst.GetOpcode()) {
                case IR::Opcode::LoadStorage32: return 4U;
                case IR::Opcode::LoadStorage64: return 8U;
                case IR::Opcode::LoadStorage128: return 16U;
                default: return 0U; // Subword fallback can read a whole U32; reject it for now.
                }
            }();
            const auto offset = extractors[*index].Offset(inst.Arg(1), alignment);
            if (width == 0 || !offset || proof.stores.size() >= 64) {
                proof.complete = false;
                continue;
            }
            proof.stores.push_back({*offset, width, width});
        }
    }
    for (auto& proof : result) {
        // Missing memory operations are NOT evidence that a descriptor is unused.
        proof.complete = proof.complete && !proof.stores.empty();
    }
    return result;
}

inline std::vector<DescriptorProof> Extract(const IR::Program& program, u32 alignment,
                                            bool allow_global_reads = false) {
    return ExtractMemory(program, alignment, false, allow_global_reads);
}

inline std::vector<DescriptorAccessProof> ExtractReadWrites(const IR::Program& program,
                                                          u32 alignment) {
    auto memory = ExtractMemory(program, alignment, true);
    std::vector<DescriptorAccessProof> result;
    result.reserve(memory.size());
    for (auto& bounds : memory) result.push_back({std::move(bounds)});
    return result;
}

// Initial runtime contract: the caller proves the full mapped WRITABLE bindings
// do not overlap. Read-only aliases do not add writes. No pointer/content heuristic
// may set this flag. It is intentionally
// required and false by default until runtime integration has its own tests.
inline std::optional<std::vector<ByteRange>> EvaluateProof(
    const DescriptorProof& proof, const Dispatch& dispatch, u64 bound_size,
    bool bindings_known_non_aliasing = false) {
    return Evaluate(proof.nodes, proof.stores, dispatch, 0, bound_size,
                    proof.complete && bindings_known_non_aliasing);
}
} // namespace Shader::StorageWriteBounds
