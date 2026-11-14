// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <bit>
#include <optional>
#include "common/container/unordered_map.h"
#include <tuple>
#include <limits>
#include <boost/container/small_vector.hpp>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/env_gate.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Optimization {
namespace {
struct TextureInst {
    ConstBufferAddr cbuf;
    IR::Inst* inst;
    IR::Block* block;
};

using TextureInstVector = boost::container::small_vector<TextureInst, 24>;

const IR::Block* ConditionalHeader(const IR::Block* body) {
    const auto predecessors{body->ImmPredecessors()};
    const auto successors{body->ImmSuccessors()};
    if (predecessors.size() != 1 || successors.size() != 1) return nullptr;
    const IR::Block* const header{predecessors[0]};
    const auto targets{header->ImmSuccessors()};
    if (targets.size() != 2 || targets[0] != body || targets[1] != successors[0] ||
        targets[1]->ImmPredecessors().size() != 2 || header->empty() ||
        header->back().GetOpcode() != IR::Opcode::ConditionRef) {
        return nullptr;
    }
    return header;
}

std::pair<IR::Value, bool> Condition(IR::Value value) {
    bool negated{};
    while (const IR::Inst* inst = value.TryInstRecursive()) {
        if (inst->GetOpcode() == IR::Opcode::LogicalNot) negated = !negated;
        else if (inst->GetOpcode() != IR::Opcode::ConditionRef) break;
        value = inst->Arg(0);
    }
    return {value.Resolve(), negated};
}

IR::Value ResolveConditionalPhi(IR::Value value, const IR::Block* block) {
    if (!value.Resolve().IsPhi() || !(block = ConditionalHeader(block))) return value;
    const auto [condition, negated]{Condition(block->back().Arg(0))};
    boost::container::small_vector<const IR::Block*, 16> visited;
    while (value.Resolve().IsPhi() && std::ranges::find(visited, block) == visited.end()) {
        visited.push_back(block);
        const IR::Inst* const phi{value.InstRecursive()};
        const auto predecessors{block->ImmPredecessors()};
        if (predecessors.size() == 1) {
            block = predecessors[0];
            continue;
        }
        if (predecessors.size() != 2 || phi->NumArgs() != 2) break;
        const IR::Block* body{predecessors[0]};
        const IR::Block* header{ConditionalHeader(body)};
        if (header != predecessors[1]) {
            body = predecessors[1];
            header = ConditionalHeader(body);
            if (header != predecessors[0]) break;
        }
        const auto [other_condition, other_negated]{Condition(header->back().Arg(0))};
        if (condition == other_condition && (phi->PhiBlock(0) == header || phi->PhiBlock(0) == body)) {
            const IR::Block* const incoming{negated == other_negated ? body : header};
            value = phi->Arg(phi->PhiBlock(0) == incoming ? 0 : 1);
        }
        block = header;
    }
    return value;
}

constexpr u32 DESCRIPTOR_SIZE = 8;
constexpr u32 DESCRIPTOR_SIZE_SHIFT = u32(std::countr_zero(DESCRIPTOR_SIZE));
constexpr u32 DESCRIPTOR_MAX_COUNT = 1024;

u32 DynamicDescriptorSizeShift(const IR::U32& dynamic_offset) {
    const IR::Inst* const inst = dynamic_offset.InstRecursive();
    if (!inst || inst->GetOpcode() != IR::Opcode::ShiftLeftLogical32)
        return DESCRIPTOR_SIZE_SHIFT;
    const IR::Value shift = inst->Arg(1);
    if (!shift.IsImmediate())
        return DESCRIPTOR_SIZE_SHIFT;
    const u32 size_shift = shift.U32();
    return size_shift >= DESCRIPTOR_SIZE_SHIFT && size_shift < 31 ? size_shift : DESCRIPTOR_SIZE_SHIFT;
}

u32 DynamicDescriptorCount(u32 base_offset, u32 size_shift, u32 max_descriptors) {
    auto const descriptor_limit = (std::max)(1U, max_descriptors);
    auto const max_cbuf_bytes = 16 * descriptor_limit;
    if (size_shift >= 31 || base_offset >= max_cbuf_bytes)
        return 1;
    auto const stride = 1U << size_shift;
    auto const available = max_cbuf_bytes - base_offset;
    if (available < DESCRIPTOR_SIZE)
        return 1;
    auto const available_count = 1U + (available - DESCRIPTOR_SIZE) / stride;
    return std::min(descriptor_limit, available_count);
}

u32 SaturatingSub(u32 lhs, u32 rhs) {
    return lhs > rhs ? lhs - rhs : 0;
}

// Whether a descriptor is an array, which the descriptor limits shrink. A texture the shader
// indexes stays one when they leave it a single element.
[[nodiscard]] bool IsDescriptorArray(const TextureBufferDescriptor& desc) noexcept {
    return desc.count > 1;
}
[[nodiscard]] bool IsDescriptorArray(const TextureDescriptor& desc) noexcept {
    return desc.count > 1 || desc.is_descriptor_array;
}

template <typename T>
[[nodiscard]] u32 StaticDescriptorCount(T const& descriptors) noexcept {
    return std::accumulate(descriptors.cbegin(), descriptors.cend(), 0U, [](auto const& acc, auto const& e) {
        return acc + (IsDescriptorArray(e) ? 0 : e.count);
    });
}

u32 DynamicSampledTextureCap(const Info& info, const HostTranslateInfo& host_info, u32 dynamic_arrays) {
    auto const sampled_limit = (std::max)(1U, std::min(host_info.max_per_stage_descriptor_sampled_images,
                                                       host_info.max_descriptor_set_sampled_images));
    // A texture is a combined image sampler: it takes a sampler as well as a sampled image.
    auto const sampler_limit = (std::max)(1U, std::min(host_info.max_per_stage_descriptor_samplers,
                                                       host_info.max_descriptor_set_samplers));
    auto const resource_limit = (std::max)(1U, host_info.max_per_stage_resources);
    if (dynamic_arrays > 0) {
        auto const sampler_static_count = StaticDescriptorCount(info.texture_descriptors);
        auto const sampled_static_count = StaticDescriptorCount(info.texture_buffer_descriptors) + sampler_static_count;
        auto const resource_static_count =
            NumDescriptors(info.constant_buffer_descriptors)
            + NumDescriptors(info.storage_buffers_descriptors)
            + sampled_static_count + NumDescriptors(info.image_buffer_descriptors)
            + NumDescriptors(info.image_descriptors);
        auto const sampled_budget = (std::min)(SaturatingSub(sampled_limit, sampled_static_count),
                                               SaturatingSub(sampler_limit, sampler_static_count));
        auto const resource_budget = SaturatingSub(resource_limit, resource_static_count);
        auto const sampled_cap = sampled_budget / dynamic_arrays;
        auto const resource_cap = resource_budget / dynamic_arrays;
        return (std::max)(1U, (std::min)(sampled_cap, resource_cap));
    }
    return (std::min)({DESCRIPTOR_MAX_COUNT, sampled_limit, sampler_limit, resource_limit});
}

// Whether opcode is an instruction past the texture pass reading a texture (or a texel buffer).
bool IsSampledImageOpcode(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::ImageSampleImplicitLod:
    case IR::Opcode::ImageSampleExplicitLod:
    case IR::Opcode::ImageSampleDrefImplicitLod:
    case IR::Opcode::ImageSampleDrefExplicitLod:
    case IR::Opcode::ImageGather:
    case IR::Opcode::ImageGatherDref:
    case IR::Opcode::ImageFetch:
    case IR::Opcode::ImageQueryDimensions:
    case IR::Opcode::ImageQueryLod:
    case IR::Opcode::ImageGradient:
        return true;
    default:
        return false;
    }
}

void MergeTextureDescriptors(IR::Program& program);

// Shrinks the texture arrays of a program to at most cap elements, clamping the element index of
// every instruction that reads an array it shrank, and merges the descriptors that the cut makes
// equal.
void CapTextureArrays(IR::Program& program, u32 cap) {
    auto& descriptors = program.info.texture_descriptors;
    boost::container::small_vector<bool, 16> shrunk(descriptors.size());
    bool any_shrunk{};
    for (size_t index = 0; index < descriptors.size(); ++index) {
        if (descriptors[index].count > cap) {
            descriptors[index].count = cap;
            if (cap == 1) {
                // One element has no stride: it takes a single texture's, which makes the array
                // equal to a descriptor of its texture the shader reads on its own.
                descriptors[index].size_shift = DESCRIPTOR_SIZE_SHIFT;
            }
            shrunk[index] = true;
            any_shrunk = true;
        }
    }
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsSampledImageOpcode(inst.GetOpcode())) {
                continue;
            }
            const auto flags = inst.Flags<IR::TextureInstInfo>();
            if (flags.type == TextureType::Buffer || inst.Arg(0).IsEmpty() ||
                !shrunk[flags.descriptor_index]) {
                continue;
            }
            const u32 count = descriptors[flags.descriptor_index].count;
            if (count == 1) {
                inst.SetArg(0, IR::Value{});
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            inst.SetArg(0, ir.UMin(IR::U32{inst.Arg(0)}, ir.Imm32(count - 1)));
        }
    }
    if (any_shrunk) {
        MergeTextureDescriptors(program);
    }
}

// The textures of a pipeline's stages. Each is a combined image sampler, which the descriptor set
// limits count as a sampler and as a sampled image (as they do a texel buffer).
struct PipelineTextures {
    u32 arrays{};             ///< Texture arrays, which can shrink
    u32 samplers{};           ///< Textures
    u32 sampled_images{};     ///< Textures and texel buffers
    u32 fixed_samplers{};     ///< Textures outside the arrays
    u32 fixed_sampled_images{}; ///< Sampled images outside the texture arrays

    explicit PipelineTextures(std::span<IR::Program* const> programs) {
        for (const IR::Program* const program : programs) {
            for (const auto& desc : program->info.texture_descriptors) {
                samplers += desc.count;
                if (IsDescriptorArray(desc)) {
                    ++arrays;
                } else {
                    fixed_samplers += desc.count;
                }
            }
            const u32 texel_buffers = NumDescriptors(program->info.texture_buffer_descriptors);
            sampled_images += texel_buffers;
            fixed_sampled_images += texel_buffers;
        }
        sampled_images += samplers;
        fixed_sampled_images += fixed_samplers;
    }

    [[nodiscard]] bool Fit(const HostTranslateInfo& host_info) const noexcept {
        return samplers <= host_info.max_descriptor_set_samplers &&
               sampled_images <= host_info.max_descriptor_set_sampled_images;
    }
};

IR::Opcode IndexedInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleImplicitLod:
        return IR::Opcode::ImageSampleImplicitLod;
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
        return IR::Opcode::ImageSampleExplicitLod;
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
        return IR::Opcode::ImageSampleDrefImplicitLod;
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
        return IR::Opcode::ImageSampleDrefExplicitLod;
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BoundImageGather:
        return IR::Opcode::ImageGather;
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BoundImageGatherDref:
        return IR::Opcode::ImageGatherDref;
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BoundImageFetch:
        return IR::Opcode::ImageFetch;
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryDimensions:
        return IR::Opcode::ImageQueryDimensions;
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BindlessImageQueryLod:
        return IR::Opcode::ImageQueryLod;
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BindlessImageGradient:
        return IR::Opcode::ImageGradient;
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BindlessImageRead:
        return IR::Opcode::ImageRead;
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BindlessImageWrite:
        return IR::Opcode::ImageWrite;
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicIAdd32:
        return IR::Opcode::ImageAtomicIAdd32;
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicSMin32:
        return IR::Opcode::ImageAtomicSMin32;
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
        return IR::Opcode::ImageAtomicUMin32;
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicSMax32:
        return IR::Opcode::ImageAtomicSMax32;
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
        return IR::Opcode::ImageAtomicUMax32;
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicInc32:
        return IR::Opcode::ImageAtomicInc32;
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicDec32:
        return IR::Opcode::ImageAtomicDec32;
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicAnd32:
        return IR::Opcode::ImageAtomicAnd32;
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicOr32:
        return IR::Opcode::ImageAtomicOr32;
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicXor32:
        return IR::Opcode::ImageAtomicXor32;
    case IR::Opcode::BoundImageAtomicExchange32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return IR::Opcode::ImageAtomicExchange32;
    default:
        return IR::Opcode::Void;
    }
}

bool IsStorageImageOpcode(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::ImageRead:
    case IR::Opcode::ImageAtomicIAdd32:
    case IR::Opcode::ImageAtomicSMin32:
    case IR::Opcode::ImageAtomicUMin32:
    case IR::Opcode::ImageAtomicSMax32:
    case IR::Opcode::ImageAtomicUMax32:
    case IR::Opcode::ImageAtomicInc32:
    case IR::Opcode::ImageAtomicDec32:
    case IR::Opcode::ImageAtomicAnd32:
    case IR::Opcode::ImageAtomicOr32:
    case IR::Opcode::ImageAtomicXor32:
    case IR::Opcode::ImageAtomicExchange32:
    case IR::Opcode::ImageWrite:
        return true;
    default:
        return false;
    }
}

// The size_shift of the descriptor of the handle at cbuf (see TextureDescriptor). The index of an
// array of handle program results is the element itself.
u32 DescriptorSizeShift(const ConstBufferAddr& cbuf) {
    return cbuf.count <= 1              ? DESCRIPTOR_SIZE_SHIFT
           : cbuf.handle_program != ~0U ? 0U
                                        : DynamicDescriptorSizeShift(cbuf.dynamic_offset);
}

TextureType ResolvedTextureType(Environment& env, const TextureInst& texture_inst,
                                IR::Opcode opcode,
                                const std::unordered_map<u32, TextureType>& handle_program_types);

u32 DynamicSampledTextureArrayCount(
    Environment& env, const TextureInstVector& to_replace,
    const std::unordered_map<u32, TextureType>& handle_program_types) {
    // Arrays, not instructions: every instruction reading an array shares its descriptor (a
    // light loop samples one table of shadow maps from dozens of places). Arrays differ where
    // their descriptors do (Descriptors::Add) but for the count, which the cap sets: two arrays
    // differing only in count share a descriptor once both reach it.
    boost::container::small_vector<
        std::tuple<u32, u32, u32, bool, u32, u32, u32, u32, u32, u32, bool>, 8>
        arrays;
    for (const TextureInst& inst : to_replace) {
        const ConstBufferAddr& cbuf{inst.cbuf};
        const IR::Opcode opcode{IndexedInstruction(*inst.inst)};
        if (cbuf.count <= 1 || IsStorageImageOpcode(opcode)) {
            continue;
        }
        // The type the replace loop gives the instruction: a size query carries none until then.
        auto flags{inst.inst->Flags<IR::TextureInstInfo>()};
        flags.type.Assign(ResolvedTextureType(env, inst, opcode, handle_program_types));
        if (flags.type != TextureType::Buffer) {
            const std::tuple array{cbuf.index,
                                   cbuf.offset,
                                   cbuf.shift_left,
                                   cbuf.has_secondary,
                                   cbuf.secondary_index,
                                   cbuf.secondary_offset,
                                   cbuf.secondary_shift_left,
                                   DescriptorSizeShift(cbuf),
                                   cbuf.handle_program,
                                   static_cast<u32>(flags.type.Value()),
                                   flags.is_depth != 0};
            if (std::ranges::find(arrays, array) == arrays.end()) {
                arrays.push_back(array);
            }
        }
    }
    return static_cast<u32>(arrays.size());
}

bool IsBindless(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BindlessImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryLod:
    case IR::Opcode::BindlessImageGradient:
    case IR::Opcode::BindlessImageRead:
    case IR::Opcode::BindlessImageWrite:
    case IR::Opcode::BindlessImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return true;
    case IR::Opcode::BoundImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BoundImageGather:
    case IR::Opcode::BoundImageGatherDref:
    case IR::Opcode::BoundImageFetch:
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BoundImageAtomicExchange32:
        return false;
    default:
        throw InvalidArgument("Invalid opcode {}", inst.GetOpcode());
    }
}

bool IsTextureInstruction(const IR::Inst& inst) {
    return IndexedInstruction(inst) != IR::Opcode::Void;
}
// Per-pass caches

static inline u32 ReadCbufCached(Environment& env, u32 index, u32 offset) {
    const CbufWordKey k{index, offset};
    if (auto it = env.cbuf_word_cache.find(k); it != env.cbuf_word_cache.end())
        return it->second;
    const u32 v = env.ReadCbufValue(index, offset);
    env.cbuf_word_cache.emplace(k, v);
    return v;
}

static inline u32 GetTextureHandleCached(Environment& env, const ConstBufferAddr& cbuf) {
    // Must all be uniquely different variables
    // If has secondary, then it will be cbuf.secondary_{index|offset}, else its 0.
    // So we can just hand it out the raw variable without using sec_idx or sec_off
    // because comparing 0 against 0 will yield true.
    const HandleKey hk{cbuf.index, cbuf.offset, cbuf.shift_left, cbuf.secondary_index, cbuf.secondary_offset, cbuf.secondary_shift_left, cbuf.count, cbuf.has_secondary};
    if (auto it = env.handle_cache.find(hk); it != env.handle_cache.end())
        return it->second;
    const u32 sec_idx = cbuf.has_secondary ? cbuf.secondary_index  : cbuf.index;
    const u32 sec_off = cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset;
    const u32 lhs = ReadCbufCached(env, cbuf.index, cbuf.offset) << cbuf.shift_left;
    const u32 rhs = ReadCbufCached(env, sec_idx, sec_off) << cbuf.secondary_shift_left;
    const u32 handle = lhs | rhs;
    env.handle_cache.emplace(hk, handle);
    return handle;
}

// Cached variants of existing helpers
// A handle computed per draw has no descriptor to read at translation time: assume an ordinary
// floating-point 2D texture, the instruction's own type deciding the rest (see the TexturePass
// ImageQueryDimensions case, which has none).
static inline TextureType ReadTextureTypeCached(Environment& env, const ConstBufferAddr& cbuf) {
    if (cbuf.handle_program != ~0U) return TextureType::Color2D;
    return env.ReadTextureType(GetTextureHandleCached(env, cbuf));
}
static inline TexturePixelFormat ReadTexturePixelFormatCached(Environment& env,
                                                                const ConstBufferAddr& cbuf) {
    if (cbuf.handle_program != ~0U) return TexturePixelFormat::A8B8G8R8_UNORM;
    return env.ReadTexturePixelFormat(GetTextureHandleCached(env, cbuf));
}
static inline bool IsTexturePixelFormatIntegerCached(Environment& env,
                                                        const ConstBufferAddr& cbuf) {
    if (cbuf.handle_program != ~0U) return false;
    return env.IsTexturePixelFormatInteger(GetTextureHandleCached(env, cbuf));
}


std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env, const HostTranslateInfo& host_info, const IR::Block* block);
static inline std::optional<ConstBufferAddr> TrackCached(const IR::Value& v, Environment& env, const HostTranslateInfo& host_info, const IR::Block* block) {
    if (const IR::Inst* key = v.TryInstRecursive()) {
        if (auto it = env.track_cache.find(key); it != env.track_cache.end()) return it->second;
        auto found = Track(v, env, host_info, block);
        if (found) env.track_cache.emplace(key, *found);
        return found;
    }
    return Track(v, env, host_info, block);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env, const HostTranslateInfo& host_info, const IR::Block* block);

bool IsMemoryLoad(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalU8: case IR::Opcode::LoadGlobalS8: case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobalS16: case IR::Opcode::LoadGlobal32: case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128: case IR::Opcode::LoadStorageU8: case IR::Opcode::LoadStorageS8:
    case IR::Opcode::LoadStorageU16: case IR::Opcode::LoadStorageS16: case IR::Opcode::LoadStorage32:
    case IR::Opcode::LoadStorage64: case IR::Opcode::LoadStorage128: case IR::Opcode::LoadLocal:
    case IR::Opcode::LoadSharedU8: case IR::Opcode::LoadSharedS8: case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16: case IR::Opcode::LoadSharedU32: case IR::Opcode::LoadSharedU64:
    case IR::Opcode::LoadSharedU128:
        return true;
    default:
        return false;
    }
}

bool IsFloatValue(const IR::Inst& inst) {
    constexpr IR::Type floats{IR::Type::F16 | IR::Type::F32 | IR::Type::F64 | IR::Type::F16x2 |
                              IR::Type::F16x3 | IR::Type::F16x4 | IR::Type::F32x2 |
                              IR::Type::F32x3 | IR::Type::F32x4 | IR::Type::F64x2 |
                              IR::Type::F64x3 | IR::Type::F64x4};
    return (inst.Type() & floats) != IR::Type::Void;
}

std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env, const HostTranslateInfo& host_info, const IR::Block* block) {
    // A value loaded from memory is not a constant buffer word, whatever its address is built
    // from: the search does not look into a load's operands (a handle read from a storage
    // buffer would otherwise be taken from the buffer's address in the constant buffer).
    const IR::Value start{ResolveConditionalPhi(value, block)};
    if (start.IsImmediate()) {
        return std::nullopt;
    }
    std::unordered_set<const IR::Inst*> visited;
    std::queue<const IR::Inst*> queue;
    queue.push(start.InstRecursive());
    visited.insert(start.InstRecursive());
    while (!queue.empty()) {
        const IR::Inst* const inst{queue.front()};
        queue.pop();
        if (const std::optional result = TryGetConstBuffer(inst, env, host_info, block)) {
            return result;
        }
        if (IsMemoryLoad(inst->GetOpcode())) {
            continue;
        }
        // Nor into a texture instruction or floating-point math: no handle is made from them,
        // while a register the program reuses for the handle and for a sample result (a Phi of
        // both) would lead the search through the sample's coordinates to an unrelated word.
        if (IsTextureInstruction(*inst) || IsFloatValue(*inst)) {
            continue;
        }
        for (size_t arg = inst->NumArgs(); arg--;) {
            const IR::Value arg_value{ResolveConditionalPhi(inst->Arg(arg), block)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            const IR::Inst* const arg_inst{arg_value.InstRecursive()};
            if (visited.insert(arg_inst).second) {
                queue.push(arg_inst);
            }
        }
    }
    return std::nullopt;
}

// Builds a handle program (shader_info.h) for a value computed from constant buffer and storage
// buffer words; false when the value depends on anything else.
thread_local IR::Opcode g_handle_failure{IR::Opcode::Void}; // the first operation not supported

bool IsUndefined(IR::Opcode opcode) {
    return opcode == IR::Opcode::UndefU1 || opcode == IR::Opcode::UndefU8 ||
           opcode == IR::Opcode::UndefU16 || opcode == IR::Opcode::UndefU32 ||
           opcode == IR::Opcode::UndefU64;
}

// Values already emitted by the program being built, by the slot that keeps them, how many slots
// are in use, and how deep in nested BuildHandleProgram calls the build is.
thread_local std::unordered_map<const IR::Inst*, u32> g_handle_slots;
thread_local u32 g_handle_slot_count{};
thread_local int g_handle_depth{};

// A handle read from a table at an index each invocation reads for itself (a light index from a
// per-pixel light list, say) has no single value for a draw. With g_handle_index_allowed, the
// build takes that index word as the element index of a descriptor array instead
// (HandleOp::Index), and g_handle_index is the word, which the shader then indexes the array
// with. Only a word read at an address no draw-wide value gives (g_handle_load_depth counts the
// reads whose address is being built), which is scaled to a byte offset (g_handle_scaled) to
// give the address of another read, qualifies, and one word per program.
thread_local bool g_handle_index_allowed{};
thread_local IR::Value g_handle_index{};
thread_local int g_handle_load_depth{};
thread_local int g_handle_scaled{};

bool BuildHandleNode(const IR::Value& value, std::vector<u32>& code, int& budget,
                     const IR::Block* block);

bool BuildHandleProgram(const IR::Value& value, std::vector<u32>& code, int& budget,
                        const IR::Block* block) {
    using Op = HandleOp;
    if (value.IsImmediate()) {
        return BuildHandleNode(value, code, budget, block);
    }
    const bool outermost = g_handle_depth++ == 0;
    if (outermost) {
        g_handle_slots.clear();
        g_handle_slot_count = 0;
    }
    SCOPE_EXIT {
        --g_handle_depth;
        if (outermost) {
            g_handle_slots.clear();
            g_handle_slot_count = 0;
        }
    };
    const IR::Inst* const inst{value.InstRecursive()};
    bool result;
    if (const auto it = g_handle_slots.find(inst); it != g_handle_slots.end()) {
        if (it->second == ~0U) {
            // Reached again while building it: a loop carries the value, which no single
            // evaluation can give.
            g_handle_failure = IR::Opcode::Phi;
            result = false;
        } else {
            code.push_back(static_cast<u32>(Op::Load));
            code.push_back(it->second);
            result = true;
        }
    } else {
        g_handle_slots.emplace(inst, ~0U);
        const size_t start = code.size();
        const u32 first_slot = g_handle_slot_count;
        const IR::Value index_before{g_handle_index};
        result = BuildHandleNode(value, code, budget, block);
        g_handle_slots.erase(inst);
        if (!result) {
            // The Tee of every value the dropped code kept goes with it: a later use of one of
            // them (another Phi input) must build it again, not load a slot nothing wrote. So
            // does an index word the dropped code took.
            g_handle_index = index_before;
            code.resize(start);
            std::erase_if(g_handle_slots, [first_slot](const auto& entry) {
                return entry.second != ~0U && entry.second >= first_slot;
            });
            g_handle_slot_count = first_slot;
        }
        if (result && inst->UseCount() > 1 && g_handle_slots.size() < 256) {
            const u32 slot = g_handle_slot_count++;
            g_handle_slots.emplace(inst, slot);
            code.push_back(static_cast<u32>(Op::Tee));
            code.push_back(slot);
        }
    }
    return result;
}

bool BuildHandleNode(const IR::Value& value, std::vector<u32>& code, int& budget,
                     const IR::Block* block) {
    if (--budget < 0) {
        return false;
    }
    using Op = HandleOp;
    const auto emit = [&](Op op) { code.push_back(static_cast<u32>(op)); };
    if (value.IsImmediate()) {
        emit(Op::Immediate);
        code.push_back(value.Type() == IR::Type::U1 ? (value.U1() ? 1U : 0U) : value.U32());
        return true;
    }
    const IR::Inst* const inst{value.InstRecursive()};
    const auto build = [&](size_t arg) { return BuildHandleProgram(inst->Arg(arg), code, budget, block); };
    const auto binary = [&](Op op) {
        if (!build(0) || !build(1)) return false;
        emit(op);
        return true;
    };
    const auto unary = [&](Op op) {
        if (!build(0)) return false;
        emit(op);
        return true;
    };
    // A word of a constant buffer read or of a storage buffer load, at byte offset +extra.
    const auto load_word = [&](const IR::Inst* source, u32 extra) {
        const auto add_extra = [&] {
            if (extra != 0) {
                emit(Op::Immediate);
                code.push_back(extra);
                emit(Op::Add);
            }
        };
        // The read's address. One for a word that gives another read's address must be
        // computable on its own: the word is the index word otherwise (see index_word).
        const auto address = [&](const IR::Value& offset) {
            const bool allowed{g_handle_index_allowed};
            const int scaled{g_handle_scaled};
            g_handle_index_allowed = allowed && g_handle_load_depth == 0;
            g_handle_scaled = 0;
            ++g_handle_load_depth;
            const bool built{BuildHandleProgram(offset, code, budget, block)};
            --g_handle_load_depth;
            g_handle_scaled = scaled;
            g_handle_index_allowed = allowed;
            return built;
        };
        // This word as the element index of a descriptor array (g_handle_index_allowed).
        const auto index_word = [&] {
            if (!g_handle_index_allowed || g_handle_load_depth == 0 || g_handle_scaled == 0 ||
                (!g_handle_index.IsEmpty() && g_handle_index.InstRecursive() != inst)) {
                return false;
            }
            g_handle_index = IR::Value{const_cast<IR::Inst*>(inst)};
            emit(Op::Index);
            return true;
        };
        switch (source->GetOpcode()) {
        case IR::Opcode::GetCbufU32:
        case IR::Opcode::GetCbufU32x2: {
            const IR::Value index{source->Arg(0)};
            const IR::Value offset{source->Arg(1)};
            if (!index.IsImmediate()) return false;
            if (offset.IsImmediate()) {
                emit(Op::Cbuf);
                code.push_back(index.U32());
                code.push_back(offset.U32() + extra);
                return true;
            }
            if (!address(offset)) return index_word();
            add_extra();
            emit(Op::CbufDynamic);
            code.push_back(index.U32());
            return true;
        }
        case IR::Opcode::LoadStorage32:
        case IR::Opcode::LoadStorage64:
        case IR::Opcode::LoadStorage128: {
            const IR::Value binding{source->Arg(0)};
            if (!binding.IsImmediate()) return false;
            if (!address(source->Arg(1))) return index_word();
            add_extra();
            emit(Op::Storage);
            code.push_back(binding.U32());
            return true;
        }
        default:
            return false;
        }
    };
    switch (inst->GetOpcode()) {
    case IR::Opcode::Identity:
    case IR::Opcode::ConditionRef:
    case IR::Opcode::ShuffleIndex: // a value made uniform (R2UR): the same in every lane
    case IR::Opcode::VoteAny:      // of a condition on per-draw words only, the same in every lane
    case IR::Opcode::VoteAll:
        return build(0);
    case IR::Opcode::GetCbufU32:
    case IR::Opcode::LoadStorage32:
        return load_word(inst, 0);
    case IR::Opcode::CompositeExtractU32x2:
    case IR::Opcode::CompositeExtractU32x3:
    case IR::Opcode::CompositeExtractU32x4: {
        // A word of a wider load (a handle in a table entry read 16 bytes at a time).
        const IR::Value element{inst->Arg(1)};
        if (!element.IsImmediate() || inst->Arg(0).IsImmediate()) {
            g_handle_failure = inst->GetOpcode();
            return false;
        }
        return load_word(inst->Arg(0).InstRecursive(), element.U32() * 4);
    }
    case IR::Opcode::Phi: {
        // Keep the incoming value that is defined; of two, the one made uniform (the
        // register-to-uniform move a conditional block makes for the handle).
        const IR::Value resolved{ResolveConditionalPhi(value, block)};
        if (resolved.IsImmediate() || resolved.InstRecursive() != inst) {
            return BuildHandleProgram(resolved, code, budget, block);
        }
        std::optional<size_t> chosen;
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            const IR::Value incoming{inst->Arg(arg)};
            if (!incoming.IsImmediate() && IsUndefined(incoming.InstRecursive()->GetOpcode())) {
                continue;
            }
            IR::Value probe{incoming};
            bool shuffled = false;
            for (int depth = 0; depth < 8 && !probe.IsImmediate(); ++depth) {
                const IR::Inst* const probe_inst{probe.InstRecursive()};
                if (probe_inst->GetOpcode() == IR::Opcode::ShuffleIndex) {
                    shuffled = true;
                    break;
                }
                if (probe_inst->GetOpcode() != IR::Opcode::Identity) break;
                probe = probe_inst->Arg(0);
            }
            if (!chosen || shuffled) chosen = arg;
        }
        if (chosen && build(*chosen)) {
            return true;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            const IR::Value incoming{inst->Arg(arg)};
            if (arg == chosen || (!incoming.IsImmediate() && IsUndefined(incoming.InstRecursive()->GetOpcode()))) {
                continue;
            }
            if (build(arg)) {
                return true;
            }
        }
        return false;
    }
    case IR::Opcode::IAdd32: return binary(Op::Add);
    case IR::Opcode::ISub32: return binary(Op::Sub);
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32: {
        // By a constant: an index scaled to a byte offset (see index_word).
        const bool is_shift{inst->GetOpcode() == IR::Opcode::ShiftLeftLogical32};
        const bool scaled{inst->Arg(1).IsImmediate() || (!is_shift && inst->Arg(0).IsImmediate())};
        g_handle_scaled += scaled ? 1 : 0;
        const bool built{build(0) && build(1)};
        g_handle_scaled -= scaled ? 1 : 0;
        if (!built) return false;
        emit(is_shift ? Op::Shl : Op::Mul);
        return true;
    }
    case IR::Opcode::BitwiseAnd32: return binary(Op::And);
    case IR::Opcode::BitwiseOr32: return binary(Op::Or);
    case IR::Opcode::BitwiseXor32: return binary(Op::Xor);
    case IR::Opcode::ShiftRightLogical32: return binary(Op::Shr);
    case IR::Opcode::ShiftRightArithmetic32: return binary(Op::Sar);
    case IR::Opcode::INeg32: return unary(Op::Neg);
    case IR::Opcode::BitwiseNot32: return unary(Op::Not);
    case IR::Opcode::UMin32: return binary(Op::UMin);
    case IR::Opcode::UMax32: return binary(Op::UMax);
    case IR::Opcode::SMin32: return binary(Op::SMin);
    case IR::Opcode::SMax32: return binary(Op::SMax);
    case IR::Opcode::SLessThan: return binary(Op::SLt);
    case IR::Opcode::SLessThanEqual: return binary(Op::SLe);
    case IR::Opcode::SGreaterThan: return binary(Op::SGt);
    case IR::Opcode::SGreaterThanEqual: return binary(Op::SGe);
    case IR::Opcode::ULessThan: return binary(Op::ULt);
    case IR::Opcode::ULessThanEqual: return binary(Op::ULe);
    case IR::Opcode::UGreaterThan: return binary(Op::UGt);
    case IR::Opcode::UGreaterThanEqual: return binary(Op::UGe);
    case IR::Opcode::IEqual: return binary(Op::Eq);
    case IR::Opcode::INotEqual: return binary(Op::Ne);
    case IR::Opcode::LogicalAnd: return binary(Op::LAnd);
    case IR::Opcode::LogicalOr: return binary(Op::LOr);
    case IR::Opcode::LogicalXor: return binary(Op::LXor);
    case IR::Opcode::LogicalNot: return unary(Op::LNot);
    case IR::Opcode::GetZeroFromOp: // the flag a producing operation sets on its result
        if (!build(0)) return false;
        emit(Op::Immediate);
        code.push_back(0);
        emit(Op::Eq);
        return true;
    case IR::Opcode::GetSignFromOp:
        if (!build(0)) return false;
        emit(Op::Immediate);
        code.push_back(0);
        emit(Op::SLt);
        return true;
    case IR::Opcode::GetCarryFromOp: {
        // The carry of a 32-bit addition: the sum below an operand.
        const IR::Value op{inst->Arg(0)};
        if (op.IsImmediate() || op.InstRecursive()->GetOpcode() != IR::Opcode::IAdd32) {
            g_handle_failure = inst->GetOpcode();
            return false;
        }
        const IR::Inst* const add{op.InstRecursive()};
        if (!BuildHandleProgram(op, code, budget, block) ||
            !BuildHandleProgram(add->Arg(0), code, budget, block)) {
            return false;
        }
        emit(Op::ULt);
        return true;
    }
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::BitCastF32U32:
        return build(0);
    case IR::Opcode::UndefU1:
    case IR::Opcode::UndefU32:
        // Read before any write on some path: no value to compute, take zero.
        emit(Op::Immediate);
        code.push_back(0);
        return true;
    case IR::Opcode::BitFieldUExtract: {
        // (base >> offset) & ((1 << count) - 1), the shift by 32 giving the all-ones mask.
        const auto mask = [&] {
            emit(Op::Immediate);
            code.push_back(1);
            if (!build(2)) return false;
            emit(Op::Shl);
            emit(Op::Immediate);
            code.push_back(1);
            emit(Op::Sub);
            return true;
        };
        if (!build(0) || !build(1)) return false;
        emit(Op::Shr);
        if (!mask()) return false;
        emit(Op::And);
        return true;
    }
    case IR::Opcode::BitFieldSExtract: {
        // (base << (32 - offset - count)) >> (32 - count), arithmetic.
        if (!build(0)) return false;
        emit(Op::Immediate);
        code.push_back(32);
        if (!build(1)) return false;
        emit(Op::Sub);
        if (!build(2)) return false;
        emit(Op::Sub);
        emit(Op::Shl);
        emit(Op::Immediate);
        code.push_back(32);
        if (!build(2)) return false;
        emit(Op::Sub);
        emit(Op::Sar);
        return true;
    }
    case IR::Opcode::BitFieldInsert: {
        // (base & ~mask) | ((insert << offset) & mask), mask = ((1 << count) - 1) << offset.
        const auto mask = [&] {
            emit(Op::Immediate);
            code.push_back(1);
            if (!build(3)) return false;
            emit(Op::Shl);
            emit(Op::Immediate);
            code.push_back(1);
            emit(Op::Sub);
            if (!build(2)) return false;
            emit(Op::Shl);
            return true;
        };
        if (!build(0) || !mask()) return false;
        emit(Op::Not);
        emit(Op::And);
        if (!build(1) || !build(2)) return false;
        emit(Op::Shl);
        if (!mask()) return false;
        emit(Op::And);
        emit(Op::Or);
        return true;
    }
    case IR::Opcode::UClamp32:
    case IR::Opcode::SClamp32: {
        const bool is_signed = inst->GetOpcode() == IR::Opcode::SClamp32;
        if (!build(0) || !build(1)) return false;
        emit(is_signed ? Op::SMax : Op::UMax);
        if (!build(2)) return false;
        emit(is_signed ? Op::SMin : Op::UMin);
        return true;
    }
    case IR::Opcode::SelectU1:
    case IR::Opcode::SelectU32:
        if (!build(0) || !build(1) || !build(2)) return false;
        emit(Op::Select);
        return true;
    default:
        g_handle_failure = inst->GetOpcode();
        return false;
    }
}

// Programs built while making the texture instructions of the pass running on this thread.
thread_local std::vector<std::vector<u32>>* g_handle_programs{};

// The operands following op in a handle program.
size_t HandleOperands(HandleOp op) {
    switch (op) {
    case HandleOp::Cbuf:
        return 2;
    case HandleOp::Immediate:
    case HandleOp::CbufDynamic:
    case HandleOp::Storage:
    case HandleOp::Tee:
    case HandleOp::Load:
        return 1;
    default:
        return 0;
    }
}

// Drops the Tee operations ending a program (they keep the result for nothing), so that two
// instructions reading the same handle share the program and the descriptor.
void TrimTrailingTees(std::vector<u32>& code) {
    using Op = HandleOp;
    size_t end{};
    for (size_t pc = 0; pc < code.size();) {
        const Op op{static_cast<Op>(code[pc++])};
        pc += HandleOperands(op);
        if (op != Op::Tee) {
            end = (std::min)(pc, code.size());
        }
    }
    code.resize(end);
}

// Elements of the descriptor array of a handle read from a table at an index word (see
// g_handle_index_allowed): the table entries a draw binds, larger indices taking the last one.
constexpr u32 HANDLE_TABLE_SIZE = 64;

// The block of each instruction and the immediate dominator of each block of the program the
// pass on this thread runs on, built when a handle program first needs them (Cooper, Harvey and
// Kennedy, over post order numbers).
class Dominance {
public:
    explicit Dominance(const IR::Program& program) : order{program.post_order_blocks} {
        for (size_t number = 0; number < order.size(); ++number) {
            numbers.emplace(order[number], number);
            for (const IR::Inst& inst : order[number]->Instructions()) {
                blocks.emplace(&inst, order[number]);
            }
        }
        idom.assign(order.size(), NONE);
        if (order.empty()) {
            return;
        }
        const size_t entry{order.size() - 1};
        idom[entry] = entry;
        const auto intersect = [this](size_t lhs, size_t rhs) {
            while (lhs != rhs) {
                while (lhs < rhs) lhs = idom[lhs];
                while (rhs < lhs) rhs = idom[rhs];
            }
            return lhs;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t number = entry; number-- > 0;) {
                size_t dominator{NONE};
                for (const IR::Block* const predecessor : order[number]->ImmPredecessors()) {
                    const auto it{numbers.find(predecessor)};
                    if (it == numbers.end() || idom[it->second] == NONE) {
                        continue;
                    }
                    dominator = dominator == NONE ? it->second : intersect(it->second, dominator);
                }
                if (dominator != idom[number]) {
                    idom[number] = dominator;
                    changed = true;
                }
            }
        }
    }

    /// Whether def runs before use on every path to use.
    [[nodiscard]] bool Dominates(const IR::Inst* def, const IR::Inst* use) const {
        const auto def_block{blocks.find(def)};
        const auto use_block{blocks.find(use)};
        if (def_block == blocks.end() || use_block == blocks.end()) {
            return false;
        }
        if (def_block->second == use_block->second) {
            for (const IR::Inst& inst : def_block->second->Instructions()) {
                if (&inst == def) return true;
                if (&inst == use) return false;
            }
            return false;
        }
        const size_t target{numbers.at(def_block->second)};
        for (size_t number = numbers.at(use_block->second); idom[number] != NONE;) {
            if (idom[number] == number) {
                return false; // the entry
            }
            number = idom[number];
            if (number == target) {
                return true;
            }
        }
        return false;
    }

private:
    static constexpr size_t NONE{~size_t{0}};
    const IR::BlockList& order;
    std::unordered_map<const IR::Block*, size_t> numbers;
    std::unordered_map<const IR::Inst*, const IR::Block*> blocks;
    std::vector<size_t> idom;
};

thread_local const IR::Program* g_program{};
thread_local std::optional<Dominance> g_dominance;

std::optional<u32> TryGetConstant(IR::Value& value, Environment& env) {
    const IR::Inst* inst = value.InstRecursive();
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32) {
        return std::nullopt;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        return std::nullopt;
    }
    if (!offset.IsImmediate()) {
        return std::nullopt;
    }
    const auto index_number = index.U32();
    if (index_number != 1) {
        return std::nullopt;
    }
    const auto offset_number = offset.U32();
    return ReadCbufCached(env, index_number, offset_number);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env, const HostTranslateInfo& host_info, const IR::Block* block) {
    switch (inst->GetOpcode()) {
    default:
        return std::nullopt;
    case IR::Opcode::BitwiseOr32: {
        std::optional lhs{TrackCached(inst->Arg(0), env, host_info, block)};
        std::optional rhs{TrackCached(inst->Arg(1), env, host_info, block)};
        if (!lhs || !rhs) {
            return std::nullopt;
        }
        if (lhs->has_secondary || rhs->has_secondary) {
            return std::nullopt;
        }
        if (lhs->count > 1 && rhs->count > 1) {
            return std::nullopt;
        }
        if (rhs->count > 1 ||
            (lhs->count == 1 &&
             (lhs->shift_left > 0 || lhs->index > rhs->index || lhs->offset > rhs->offset))) {
            std::swap(lhs, rhs);
        }
        return ConstBufferAddr{
            .index = lhs->index,
            .offset = lhs->offset,
            .shift_left = lhs->shift_left,
            .secondary_index = rhs->index,
            .secondary_offset = rhs->offset,
            .secondary_shift_left = rhs->shift_left,
            .dynamic_offset = lhs->dynamic_offset,
            .count = lhs->count,
            .has_secondary = true,
        };
    }
    case IR::Opcode::ShiftLeftLogical32: {
        const IR::Value shift{inst->Arg(1)};
        if (!shift.IsImmediate()) {
            return std::nullopt;
        }
        std::optional lhs{TrackCached(inst->Arg(0), env, host_info, block)};
        if (lhs) {
            lhs->shift_left = shift.U32();
        }
        return lhs;
    }
    case IR::Opcode::BitwiseAnd32: {
        IR::Value op1{inst->Arg(0)};
        IR::Value op2{inst->Arg(1)};
        if (op1.IsImmediate()) {
            std::swap(op1, op2);
        }
        if (!op2.IsImmediate() && !op1.IsImmediate()) {
            do {
                auto try_index = TryGetConstant(op1, env);
                if (try_index) {
                    op1 = op2;
                    op2 = IR::Value{*try_index};
                    break;
                }
                auto try_index_2 = TryGetConstant(op2, env);
                if (try_index_2) {
                    op2 = IR::Value{*try_index_2};
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        std::optional lhs{TrackCached(op1, env, host_info, block)};
        if (lhs) {
            lhs->shift_left = static_cast<u32>(std::countr_zero(op2.U32()));
        }
        return lhs;
        break;
    }
    case IR::Opcode::GetCbufU32x2:
    case IR::Opcode::GetCbufU32:
        break;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        // Reading a bindless texture from variable indices is valid
        // but not supported here at the moment
        return std::nullopt;
    }
    if (offset.IsImmediate()) {
        return ConstBufferAddr{
            .index = index.U32(),
            .offset = offset.U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    IR::Inst* const offset_inst{offset.InstRecursive()};
    u32 base_offset{};
    IR::U32 dynamic_offset{offset};
    if (offset_inst->GetOpcode() == IR::Opcode::IAdd32) {
        if (offset_inst->Arg(0).IsImmediate()) {
            base_offset = offset_inst->Arg(0).U32();
            dynamic_offset = IR::U32{offset_inst->Arg(1)};
        } else if (offset_inst->Arg(1).IsImmediate()) {
            base_offset = offset_inst->Arg(1).U32();
            dynamic_offset = IR::U32{offset_inst->Arg(0)};
        } else {
            return std::nullopt;
        }
    } else if (offset_inst->GetOpcode() != IR::Opcode::ShiftLeftLogical32) {
        return std::nullopt;
    }
    auto const size_shift = DynamicDescriptorSizeShift(dynamic_offset);
    auto const sampled_limit = (std::max)(1U, (std::min)(host_info.max_per_stage_descriptor_sampled_images,
                                                         host_info.max_descriptor_set_sampled_images));
    auto const resource_limit = (std::max)(1U, host_info.max_per_stage_resources);
    return ConstBufferAddr{
        .index = index.U32(),
        .offset = base_offset,
        .shift_left = 0,
        .secondary_index = 0,
        .secondary_offset = 0,
        .secondary_shift_left = 0,
        .dynamic_offset = dynamic_offset,
        .count = DynamicDescriptorCount(base_offset, size_shift, (std::min)({DESCRIPTOR_MAX_COUNT, sampled_limit, resource_limit})),
        .has_secondary = false,
    };
}

TextureInst MakeInst(Environment& env, IR::Block* block, IR::Inst& inst, const HostTranslateInfo& host_info) {
    ConstBufferAddr addr;
    if (IsBindless(inst)) {
        const std::optional<ConstBufferAddr> track_addr{TrackCached(inst.Arg(0), env, host_info, block)};

        if (track_addr) {
            addr = *track_addr;
        } else {
            // Read from a storage buffer (a handle table): computed for each draw.
            std::vector<u32> code;
            int budget = 4096;
            g_handle_failure = IR::Opcode::Void;
            bool built{g_handle_programs && BuildHandleProgram(inst.Arg(0), code, budget, block)};
            IR::Value index{};
            // Only sampled textures: an array of storage images would have every element taken
            // as written.
            const bool sampled{!IsStorageImageOpcode(IndexedInstruction(inst)) &&
                               inst.Flags<IR::TextureInstInfo>().type != TextureType::Buffer};
            if (!built && g_handle_programs && sampled && host_info.support_handle_table_arrays) {
                // Read from a table at an index each invocation reads for itself: a descriptor
                // array of the table's entries, which the shader indexes with that index.
                code.clear();
                budget = 4096;
                g_handle_index = IR::Value{};
                g_handle_index_allowed = true;
                built = BuildHandleProgram(inst.Arg(0), code, budget, block);
                g_handle_index_allowed = false;
                index = built ? g_handle_index : IR::Value{};
                g_handle_index = IR::Value{};
            }
            if (!built) {
                // Not computable for a whole draw (a handle that changes in a loop): keep the
                // earlier search through loads, which at least gives the shader a texture.
                const std::optional<ConstBufferAddr> legacy{IR::BreadthFirstSearch(inst.Arg(0),
                    [&env, &host_info, block](const IR::Inst* candidate) {
                        return TryGetConstBuffer(candidate, env, host_info, block);
                    }, [&](IR::Value operand) { return ResolveConditionalPhi(operand, block); })};
                if (!legacy) {
                    throw NotImplementedException("Failed to track bindless texture constant buffer (stopped at {}, budget {})",
                                                  g_handle_failure, budget);
                }
                static std::atomic<int> warnings{};
                if (warnings.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_WARNING(Shader, "Bindless {} handle not computable per draw (stopped at {}): "
                                "taking cbuf{}+{:#x}, likely not the texture the shader reads",
                                inst.GetOpcode(), g_handle_failure, legacy->index, legacy->offset);
                }
                return TextureInst{.cbuf = *legacy, .inst = &inst, .block = block};
            }
            TrimTrailingTees(code);
            auto& programs{*g_handle_programs};
            auto it{std::ranges::find(programs, code)};
            if (it == programs.end()) {
                programs.push_back(std::move(code));
                it = programs.end() - 1;
            }
            // The shader indexes the array with the index word, which must be defined wherever
            // the texture instruction runs; where it isn't, every invocation takes the first
            // element.
            u32 count{1};
            if (!index.IsEmpty() && g_program != nullptr) {
                if (!g_dominance) {
                    g_dominance.emplace(*g_program);
                }
                if (g_dominance->Dominates(index.InstRecursive(), &inst)) {
                    count = HANDLE_TABLE_SIZE;
                }
            }
            addr = ConstBufferAddr{
                .index = 0,
                .offset = 0,
                .shift_left = 0,
                .secondary_index = 0,
                .secondary_offset = 0,
                .secondary_shift_left = 0,
                .dynamic_offset = count > 1 ? IR::U32{index} : IR::U32{},
                .count = count,
                .has_secondary = false,
                .handle_program = static_cast<u32>(it - programs.begin()),
            };
        }
    } else {
        addr = ConstBufferAddr{
            .index = env.TextureBoundBuffer(),
            .offset = inst.Arg(0).U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    return TextureInst{
        .cbuf = addr,
        .inst = &inst,
        .block = block,
    };
}

u32 GetTextureHandle(Environment& env, const ConstBufferAddr& cbuf) {
    const u32 secondary_index{cbuf.has_secondary ? cbuf.secondary_index : cbuf.index};
    const u32 secondary_offset{cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset};
    const u32 lhs_raw{env.ReadCbufValue(cbuf.index, cbuf.offset) << cbuf.shift_left};
    const u32 rhs_raw{env.ReadCbufValue(secondary_index, secondary_offset)
                      << cbuf.secondary_shift_left};
    return lhs_raw | rhs_raw;
}

[[maybe_unused]] TextureType ReadTextureType(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTextureType(GetTextureHandle(env, cbuf));
}

[[maybe_unused]] TexturePixelFormat ReadTexturePixelFormat(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTexturePixelFormat(GetTextureHandle(env, cbuf));
}

[[maybe_unused]] bool IsTexturePixelFormatInteger(Environment& env, const ConstBufferAddr& cbuf) {
    return env.IsTexturePixelFormatInteger(GetTextureHandle(env, cbuf));
}

class Descriptors {
public:
    explicit Descriptors(TextureBufferDescriptors& texture_buffer_descriptors_,
                         ImageBufferDescriptors& image_buffer_descriptors_,
                         TextureDescriptors& texture_descriptors_,
                         ImageDescriptors& image_descriptors_)
        : texture_buffer_descriptors{texture_buffer_descriptors_},
          image_buffer_descriptors{image_buffer_descriptors_},
          texture_descriptors{texture_descriptors_}, image_descriptors{image_descriptors_} {}

    u32 Add(const TextureBufferDescriptor& desc) {
        return Add(texture_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.count == existing.count && desc.size_shift == existing.size_shift &&
                   desc.has_secondary == existing.has_secondary &&
                   desc.handle_program == existing.handle_program;
        });
    }

    u32 Add(const ImageBufferDescriptor& desc) {
        const u32 index{Add(image_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.format == existing.format && desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift &&
                   desc.handle_program == existing.handle_program;
        })};
        image_buffer_descriptors[index].is_written |= desc.is_written;
        image_buffer_descriptors[index].is_read |= desc.is_read;
        image_buffer_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

    u32 Add(const TextureDescriptor& desc) {
        const u32 index{Add(texture_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.is_depth == existing.is_depth &&
                   desc.has_secondary == existing.has_secondary &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.count == existing.count && desc.size_shift == existing.size_shift &&
                   desc.handle_program == existing.handle_program;
        })};
        // TODO: Read this from TIC
        texture_descriptors[index].is_multisample |= desc.is_multisample;
        texture_descriptors[index].is_integer |= desc.is_integer;
        texture_descriptors[index].is_descriptor_array |= desc.is_descriptor_array;
        return index;
    }

    u32 Add(const ImageDescriptor& desc) {
        const u32 index{Add(image_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.format == existing.format &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift &&
                   desc.handle_program == existing.handle_program;
        })};
        image_descriptors[index].is_written |= desc.is_written;
        image_descriptors[index].is_read |= desc.is_read;
        image_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        // TODO: Handle arrays
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    TextureBufferDescriptors& texture_buffer_descriptors;
    ImageBufferDescriptors& image_buffer_descriptors;
    TextureDescriptors& texture_descriptors;
    ImageDescriptors& image_descriptors;
};

// Keeps one of the texture descriptors that bind the same textures, as the pass adds them: a cut
// can leave an array with the elements of another descriptor of its texture, or with the single
// texture a descriptor of its own reads. Points the instructions at the descriptors kept.
void MergeTextureDescriptors(IR::Program& program) {
    Info& info{program.info};
    TextureDescriptors merged;
    Descriptors descriptors{info.texture_buffer_descriptors, info.image_buffer_descriptors, merged,
                            info.image_descriptors};
    JoinedDescriptors indices;
    indices.textures.reserve(info.texture_descriptors.size());
    for (const TextureDescriptor& desc : info.texture_descriptors) {
        indices.textures.push_back(descriptors.Add(desc));
    }
    if (merged.size() == info.texture_descriptors.size()) {
        return;
    }
    info.texture_descriptors = std::move(merged);
    RemapTextureDescriptors(program, indices);
}

// Expects the sample's descriptor to be assigned: the size query reads the same texture (and
// the same element of a descriptor array).
void PatchImageSampleImplicitLod(IR::Block& block, IR::Inst& inst) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const IR::Value coord(inst.Arg(1));
    const IR::Value handle(inst.Arg(0).IsEmpty() ? IR::Value{ir.Imm32(0)} : inst.Arg(0));
    const IR::U32 lod{ir.Imm32(0)};
    const IR::U1 skip_mips{ir.Imm1(true)};
    const IR::Value texture_size = ir.ImageQueryDimension(handle, lod, skip_mips, info);
    inst.SetArg(
        1, ir.CompositeConstruct(
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 0)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 0)))),
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 1)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 1))))));
}

bool IsPixelFormatSNorm(TexturePixelFormat pixel_format) {
    switch (pixel_format) {
    case TexturePixelFormat::A8B8G8R8_SNORM:
    case TexturePixelFormat::R8G8_SNORM:
    case TexturePixelFormat::R8_SNORM:
    case TexturePixelFormat::R16G16B16A16_SNORM:
    case TexturePixelFormat::R16G16_SNORM:
    case TexturePixelFormat::R16_SNORM:
        return true;
    default:
        return false;
    }
}

void PatchTexelFetch(IR::Block& block, IR::Inst& inst, TexturePixelFormat pixel_format) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    auto get_max_value = [pixel_format]() -> float {
        switch (pixel_format) {
        case TexturePixelFormat::A8B8G8R8_SNORM:
        case TexturePixelFormat::R8G8_SNORM:
        case TexturePixelFormat::R8_SNORM:
            return 1.f / (std::numeric_limits<char>::max)();
        case TexturePixelFormat::R16G16B16A16_SNORM:
        case TexturePixelFormat::R16G16_SNORM:
        case TexturePixelFormat::R16_SNORM:
            return 1.f / (std::numeric_limits<short>::max)();
        default:
            throw InvalidArgument("Invalid texture pixel format");
        }
    };

    const IR::Value new_inst{&*block.PrependNewInst(it, inst)};
    const IR::F32 x(ir.CompositeExtract(new_inst, 0));
    const IR::F32 y(ir.CompositeExtract(new_inst, 1));
    const IR::F32 z(ir.CompositeExtract(new_inst, 2));
    const IR::F32 w(ir.CompositeExtract(new_inst, 3));
    const IR::F16F32F64 max_value(ir.Imm32(get_max_value()));
    const IR::Value converted =
        ir.CompositeConstruct(ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(x)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(y)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(z)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(w)), max_value));
    inst.ReplaceUsesWith(converted);
}

// The type TexturePass gives the texture an instruction reads, opcode being its indexed one. A
// size query carries none: it takes that of another instruction with the same handle program, or
// the one its handle reads as.
TextureType ResolvedTextureType(Environment& env, const TextureInst& texture_inst,
                                IR::Opcode opcode,
                                const std::unordered_map<u32, TextureType>& handle_program_types) {
    const ConstBufferAddr& cbuf{texture_inst.cbuf};
    const TextureType type{texture_inst.inst->Flags<IR::TextureInstInfo>().type.Value()};
    switch (opcode) {
    case IR::Opcode::ImageQueryDimensions:
        if (const auto it{handle_program_types.find(cbuf.handle_program)};
            it != handle_program_types.end()) {
            return it->second;
        }
        return ReadTextureTypeCached(env, cbuf);
    case IR::Opcode::ImageFetch:
        // Replace with the bound texture type only when it's a texture buffer
        // If the instruction is 1D and the bound type is 2D, don't change the code and let
        // the rasterizer robustness handle it
        // This happens on Fire Emblem: Three Houses
        if (type == TextureType::Color1D &&
            ReadTextureTypeCached(env, cbuf) == TextureType::Buffer) {
            return TextureType::Buffer;
        }
        return type;
    default:
        return type;
    }
}
} // Anonymous namespace

void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info) {
    // reset per-pass caches
    env.cbuf_word_cache.clear();
    env.handle_cache.clear();
    env.track_cache.clear();
    TextureInstVector to_replace;
    g_handle_programs = &program.info.handle_programs;
    g_program = &program;
    g_dominance.reset();
    SCOPE_EXIT {
        g_program = nullptr;
        g_dominance.reset();
    };
    for (IR::Block* const block : program.post_order_blocks) {
        env.track_cache.clear();
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInstruction(inst)) {
                continue;
            }
            to_replace.push_back(MakeInst(env, block, inst, host_info));
        }
    }
    // Sort instructions to visit textures by constant buffer index, then by offset
    std::ranges::sort(to_replace, [](const auto& a, const auto& b) {
        if (a.cbuf.index != b.cbuf.index) return a.cbuf.index < b.cbuf.index;
        return a.cbuf.offset < b.cbuf.offset;
    });
    Descriptors descriptors{
        program.info.texture_buffer_descriptors,
        program.info.image_buffer_descriptors,
        program.info.texture_descriptors,
        program.info.image_descriptors,
    };
    bool has_last_is_integer{false};
    u32 last_cbuf_index{};
    u32 last_cbuf_offset{};
    bool last_is_integer{false};
    const auto is_texture_pixel_format_integer{[&](const ConstBufferAddr& cbuf_addr) {
        if (cbuf_addr.handle_program != ~0U) {
            // Every handle program is at cbuf 0 offset 0: the cache below can't tell them apart.
            return IsTexturePixelFormatIntegerCached(env, cbuf_addr);
        }
        if (has_last_is_integer && last_cbuf_index == cbuf_addr.index &&
            last_cbuf_offset == cbuf_addr.offset) {
            return last_is_integer;
        }
        last_is_integer = IsTexturePixelFormatIntegerCached(env, cbuf_addr);
        last_cbuf_index = cbuf_addr.index;
        last_cbuf_offset = cbuf_addr.offset;
        has_last_is_integer = true;
        return last_is_integer;
    }};
    // A handle program has no descriptor to read a type from, and ImageQueryDimensions carries
    // none: it takes the type of another instruction using the same handle.
    std::unordered_map<u32, TextureType> handle_program_types;
    for (const TextureInst& texture_inst : to_replace) {
        const IR::Opcode opcode{IndexedInstruction(*texture_inst.inst)};
        if (texture_inst.cbuf.handle_program != ~0U && opcode != IR::Opcode::ImageQueryDimensions &&
            !IsStorageImageOpcode(opcode)) {
            handle_program_types.try_emplace(
                texture_inst.cbuf.handle_program,
                texture_inst.inst->Flags<IR::TextureInstInfo>().type.Value());
        }
    }
    const u32 sampled_dynamic_cap = DynamicSampledTextureCap(
        program.info, host_info,
        DynamicSampledTextureArrayCount(env, to_replace, handle_program_types));
    for (TextureInst& texture_inst : to_replace) {
        // TODO: Handle arrays
        IR::Inst* const inst{texture_inst.inst};
        inst->ReplaceOpcode(IndexedInstruction(*inst));

        const auto& cbuf{texture_inst.cbuf};
        auto flags{inst->Flags<IR::TextureInstInfo>()};
        bool is_multisample{false};
        bool is_rect_sample{false};
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageQueryDimensions:
            flags.type.Assign(
                ResolvedTextureType(env, texture_inst, inst->GetOpcode(), handle_program_types));
            inst->SetFlags(flags);
            break;
        case IR::Opcode::ImageSampleImplicitLod:
            if (flags.type != TextureType::Color2D) {
                break;
            }
            // Normalized once the descriptor is known (below).
            is_rect_sample = ReadTextureTypeCached(env, cbuf) == TextureType::Color2DRect;
            break;
        case IR::Opcode::ImageFetch:
            if (flags.type == TextureType::Color2D || flags.type == TextureType::Color2DRect ||
                flags.type == TextureType::ColorArray2D) {
                is_multisample = !inst->Arg(4).IsEmpty();
            } else {
                inst->SetArg(4, IR::U32{});
            }
            flags.type.Assign(
                ResolvedTextureType(env, texture_inst, inst->GetOpcode(), handle_program_types));
            break;
        default:
            break;
        }
        u32 index;
        const u32 size_shift{DescriptorSizeShift(cbuf)};
        u32 count = cbuf.count;
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageAtomicIAdd32:
        case IR::Opcode::ImageAtomicSMin32:
        case IR::Opcode::ImageAtomicUMin32:
        case IR::Opcode::ImageAtomicSMax32:
        case IR::Opcode::ImageAtomicUMax32:
        case IR::Opcode::ImageAtomicInc32:
        case IR::Opcode::ImageAtomicDec32:
        case IR::Opcode::ImageAtomicAnd32:
        case IR::Opcode::ImageAtomicOr32:
        case IR::Opcode::ImageAtomicXor32:
        case IR::Opcode::ImageAtomicExchange32:
        case IR::Opcode::ImageWrite: {
            if (cbuf.has_secondary) {
                throw NotImplementedException("Unexpected separate sampler");
            }
            const bool is_written{inst->GetOpcode() != IR::Opcode::ImageRead};
            const bool is_read{inst->GetOpcode() != IR::Opcode::ImageWrite};
            const bool is_integer{is_texture_pixel_format_integer(cbuf)};
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(ImageBufferDescriptor{
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = count,
                    .size_shift = size_shift,
                    .handle_program = cbuf.handle_program,
                });
            } else {
                index = descriptors.Add(ImageDescriptor{
                    .type = flags.type,
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = count,
                    .size_shift = size_shift,
                    .handle_program = cbuf.handle_program,
                });
            }
            break;
        }
        default:
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(TextureBufferDescriptor{
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = count,
                    .size_shift = size_shift,
                    .handle_program = cbuf.handle_program,
                });
            } else {
                count = std::min(count, sampled_dynamic_cap);
                // Depth-compare sampling returns a float in Vulkan, so its image must be float
                // even when a garbage or unknown descriptor reads as an integer format.
                static const bool depth_float_fix{Common::EnvGate("EGLANTINE_DEPTH_FLOAT_FIX")};
                const bool is_integer{(!depth_float_fix || flags.is_depth == 0) &&
                                      is_texture_pixel_format_integer(cbuf)};
                index = descriptors.Add(TextureDescriptor{
                    .type = flags.type,
                    .is_depth = flags.is_depth != 0,
                    .is_multisample = is_multisample,
                    .is_integer = is_integer,
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = count,
                    // One element has no stride (see CapTextureArrays).
                    .size_shift = count > 1 ? size_shift : DESCRIPTOR_SIZE_SHIFT,
                    .handle_program = cbuf.handle_program,
                    .is_descriptor_array = cbuf.count > 1,
                });
                flags.is_integer.Assign(is_integer ? 1 : 0);
            }
            break;
        }
        flags.descriptor_index.Assign(index);
        inst->SetFlags(flags);

        if (count > 1) {
            const auto insert_point{IR::Block::InstructionList::s_iterator_to(*inst)};
            IR::IREmitter ir{*texture_inst.block, insert_point};
            const IR::U32 element{size_shift != 0
                                      ? IR::U32{ir.ShiftRightLogical(cbuf.dynamic_offset, ir.Imm32(size_shift))}
                                      : cbuf.dynamic_offset};
            inst->SetArg(0, ir.UMin(element, ir.Imm32(count - 1)));
        } else {
            inst->SetArg(0, IR::Value{});
        }
        if (is_rect_sample) {
            PatchImageSampleImplicitLod(*texture_inst.block, *texture_inst.inst);
        }

        if (!host_info.support_snorm_render_buffer && inst->GetOpcode() == IR::Opcode::ImageFetch &&
            flags.type == TextureType::Buffer) {
            const auto pixel_format = ReadTexturePixelFormatCached(env, cbuf);
            if (IsPixelFormatSNorm(pixel_format)) {
                PatchTexelFetch(*texture_inst.block, *texture_inst.inst, pixel_format);
            }
        }
    }
}

void LimitSampledTextureArrayElements(IR::Program& program, u32 cap) {
    CapTextureArrays(program, cap);
}

void LimitSampledTextureArrays(IR::Program& program, const HostTranslateInfo& host_info) {
    auto& info = program.info;
    // Arrays a cap (the pass's, or this one's for a half of a joined stage) has cut to a single
    // element count too: the stage must still fit with them.
    const u32 arrays = static_cast<u32>(std::ranges::count_if(
        info.texture_descriptors, [](const auto& desc) { return IsDescriptorArray(desc); }));
    if (arrays == 0) {
        return;
    }
    const u32 buffers_and_images = NumDescriptors(info.constant_buffer_descriptors) +
                                   NumDescriptors(info.storage_buffers_descriptors) +
                                   NumDescriptors(info.image_descriptors) +
                                   NumDescriptors(info.image_buffer_descriptors);
    // Textures are combined image samplers, counted as samplers too; texel buffers are not.
    const auto fit = [&](u32 samplers, u32 sampled) {
        return sampled <= host_info.max_per_stage_descriptor_sampled_images &&
               sampled <= host_info.max_descriptor_set_sampled_images &&
               samplers <= host_info.max_per_stage_descriptor_samplers &&
               samplers <= host_info.max_descriptor_set_samplers &&
               sampled + buffers_and_images <= host_info.max_per_stage_resources;
    };
    const u32 original_samplers = NumDescriptors(info.texture_descriptors);
    if (fit(original_samplers,
            original_samplers + NumDescriptors(info.texture_buffer_descriptors))) {
        return;
    }
    CapTextureArrays(program, DynamicSampledTextureCap(info, host_info, arrays));
    const u32 samplers = NumDescriptors(info.texture_descriptors);
    const u32 sampled = samplers + NumDescriptors(info.texture_buffer_descriptors);
    if (!fit(samplers, sampled)) {
        throw NotImplementedException(
            "Texture arrays exceed descriptor limits: samplers={}, sampled={}, resources={}",
            samplers, sampled, sampled + buffers_and_images);
    }
}

void LimitPipelineSampledTextureArrays(std::span<IR::Program* const> programs,
                                       const HostTranslateInfo& host_info) {
    const PipelineTextures textures{programs};
    if (textures.arrays == 0 || textures.Fit(host_info)) {
        return;
    }
    const u32 budget = (std::min)(
        SaturatingSub(host_info.max_descriptor_set_samplers, textures.fixed_samplers),
        SaturatingSub(host_info.max_descriptor_set_sampled_images, textures.fixed_sampled_images));
    const u32 cap = (std::max)(1U, budget / textures.arrays);
    for (IR::Program* const program : programs) {
        CapTextureArrays(*program, cap);
    }
    const PipelineTextures capped{programs};
    if (!capped.Fit(host_info)) {
        throw NotImplementedException(
            "Texture arrays exceed the pipeline's descriptor limits: samplers={}, sampled={}",
            capped.samplers, capped.sampled_images);
    }
}

void LimitSharedStageSampledTextureArrays(std::span<IR::Program* const> programs,
                                          const HostTranslateInfo& host_info,
                                          u32 auxiliary_resources) {
    u32 other_resources{auxiliary_resources};
    for (const IR::Program* const program : programs) {
        const Info& info{program->info};
        other_resources += NumDescriptors(info.constant_buffer_descriptors) +
                           NumDescriptors(info.storage_buffers_descriptors) +
                           NumDescriptors(info.image_buffer_descriptors) +
                           NumDescriptors(info.image_descriptors);
    }
    // The pipeline limiter already counts every texture array and texel buffer across programs.
    // Give it the budget of their shared host stage, after the non-sampled resources.
    HostTranslateInfo shared{host_info};
    shared.max_descriptor_set_samplers = (std::min)(host_info.max_descriptor_set_samplers,
                                                    host_info.max_per_stage_descriptor_samplers);
    shared.max_descriptor_set_sampled_images =
        (std::min)({host_info.max_descriptor_set_sampled_images,
                    host_info.max_per_stage_descriptor_sampled_images,
                    SaturatingSub(host_info.max_per_stage_resources, other_resources)});
    LimitPipelineSampledTextureArrays(programs, shared);
}

JoinedDescriptors JoinTextureInfo(Info& base, Info& source, std::span<const u32> storage_bindings) {
    base.requires_coherent_image_memory |= source.requires_coherent_image_memory;
    // The handle programs of source join those of base (once each, as the pass keeps them),
    // reading its storage buffers at their bindings in base.
    std::vector<u32> program_index(source.handle_programs.size());
    for (size_t program = 0; program < source.handle_programs.size(); ++program) {
        auto code{source.handle_programs[program]};
        for (size_t pc = 0; pc < code.size();) {
            const HandleOp op{static_cast<HandleOp>(code[pc++])};
            if (op == HandleOp::Storage && pc < code.size() &&
                code[pc] < storage_bindings.size()) {
                code[pc] = storage_bindings[code[pc]];
            }
            pc += HandleOperands(op);
        }
        auto it{std::ranges::find(base.handle_programs, code)};
        if (it == base.handle_programs.end()) {
            base.handle_programs.push_back(std::move(code));
            it = base.handle_programs.end() - 1;
        }
        program_index[program] = static_cast<u32>(it - base.handle_programs.begin());
    }
    Descriptors descriptors{
        base.texture_buffer_descriptors,
        base.image_buffer_descriptors,
        base.texture_descriptors,
        base.image_descriptors,
    };
    JoinedDescriptors joined;
    const auto add = [&](auto desc, std::vector<u32>& indices) {
        if (desc.handle_program != ~0U) {
            desc.handle_program = program_index[desc.handle_program];
        }
        indices.push_back(descriptors.Add(desc));
    };
    for (auto& desc : source.texture_buffer_descriptors) {
        add(desc, joined.texture_buffers);
    }
    for (auto& desc : source.image_buffer_descriptors) {
        add(desc, joined.image_buffers);
    }
    for (auto& desc : source.texture_descriptors) {
        add(desc, joined.textures);
    }
    for (auto& desc : source.image_descriptors) {
        add(desc, joined.images);
    }
    return joined;
}

void RemapTextureDescriptors(IR::Program& program, const JoinedDescriptors& descriptors) {
    const auto remap = [](const std::vector<u32>& indices, u32 index) {
        return index < indices.size() ? indices[index] : index;
    };
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode opcode{inst.GetOpcode()};
            if (opcode == IR::Opcode::IsTextureScaled || opcode == IR::Opcode::IsImageScaled) {
                // The rescaling pass's test of a descriptor, which the pipeline's rescaling bits
                // number as the texture and image descriptors are.
                if (inst.Arg(0).IsImmediate()) {
                    const auto& indices{opcode == IR::Opcode::IsTextureScaled ? descriptors.textures
                                                                              : descriptors.images};
                    inst.SetArg(0, IR::Value{remap(indices, inst.Arg(0).U32())});
                }
                continue;
            }
            const bool is_image{IsStorageImageOpcode(opcode)};
            if (!is_image && !IsSampledImageOpcode(opcode)) {
                continue;
            }
            // The lists the texture pass took the descriptor from.
            auto flags{inst.Flags<IR::TextureInstInfo>()};
            const bool is_buffer{flags.type == TextureType::Buffer};
            const auto& indices{
                is_image ? (is_buffer ? descriptors.image_buffers : descriptors.images)
                         : (is_buffer ? descriptors.texture_buffers : descriptors.textures)};
            flags.descriptor_index.Assign(remap(indices, flags.descriptor_index));
            inst.SetFlags(flags);
        }
    }
}

} // namespace Shader::Optimization
