// SPDX-FileCopyrightText: Copyright 2026 sudachi Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/shader_info.h"

// Games often pass the clip space position to the fragment shader through a generic varying and
// divide it by its w to get the screen position (e.g. to fetch the depth buffer at the current
// pixel). On the guest GPU the fragment shader interpolates the original vertex values and the
// division is exact. Some host GPUs (AMD) clip huge primitives crossing the w = 0 plane and
// interpolate the varyings of the clipped vertices with much less precision than the position
// used for rasterization: in Xenoblade Chronicles 2 the screen position computed this way is off
// by up to 1.5 pixels, so depth fetches hit neighbour texels.
//
// When the vertex shader stores the same value (or the same value plus a draw-uniform offset) in
// the x, y and w components of a generic varying and of the position, the fragment shader can
// rebuild those components exactly from gl_FragCoord and the host viewport. Uniform offsets are
// moved to a free flat varying.

namespace Shader::Optimization {
namespace {
constexpr std::array<size_t, 3> RECONSTRUCTED_COMPONENTS{0, 1, 3};

struct Store {
    IR::Block* block{};
    IR::Inst* inst{};
    IR::Value value;
    u32 count{};
};

IR::Value Resolve(const IR::Value& value) {
    return value.IsImmediate() ? value : value.Resolve();
}

bool SameValue(const IR::Value& lhs, const IR::Value& rhs) {
    const IR::Value a{Resolve(lhs)};
    const IR::Value b{Resolve(rhs)};
    if (a.IsImmediate() != b.IsImmediate()) {
        return false;
    }
    if (a.IsImmediate()) {
        return a.Type() == b.Type() && a.U32() == b.U32();
    }
    return a.InstRecursive() == b.InstRecursive();
}

/// Values that are the same for every vertex of a draw (built from immediates and constant
/// buffers only)
class UniformChecker {
public:
    bool IsUniform(const IR::Value& value, u32 depth = 0) {
        if (value.IsImmediate()) {
            return true;
        }
        IR::Inst* const inst{value.InstRecursive()};
        if (const auto it = cache.find(inst); it != cache.end()) {
            return it->second;
        }
        bool uniform = depth < 64 && !IsVaryingSource(inst->GetOpcode());
        for (size_t arg = 0; uniform && arg < inst->NumArgs(); ++arg) {
            const IR::Value operand{inst->Arg(arg)};
            if (operand.IsImmediate()) {
                continue;
            }
            if (operand.Type() == IR::Type::Attribute) {
                uniform = false;
                break;
            }
            uniform = IsUniform(operand, depth + 1);
        }
        cache.emplace(inst, uniform);
        return uniform;
    }

private:
    static bool IsVaryingSource(IR::Opcode opcode) {
        if (opcode == IR::Opcode::Phi) {
            return true;
        }
        const std::string_view name{IR::NameOf(opcode)};
        for (const std::string_view prefix : {"GetAttribute", "Load", "Image", "Bound", "Bindless",
                                              "Global", "Storage", "Shared", "Shuffle", "Vote",
                                              "Subgroup", "Lane", "Undef"}) {
            if (name.starts_with(prefix)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_map<IR::Inst*, bool> cache;
};

/// Returns the offset of position over attribute (position = attribute + offset) when it is draw
/// uniform. std::nullopt when the position isn't derived from the attribute that way, and an
/// empty value when both are the same value.
std::optional<IR::Value> PositionOffset(UniformChecker& checker, const IR::Value& attribute,
                                        const IR::Value& position) {
    if (SameValue(attribute, position)) {
        return IR::Value{};
    }
    const IR::Value resolved{Resolve(position)};
    if (resolved.IsImmediate()) {
        return std::nullopt;
    }
    IR::Inst* const inst{resolved.InstRecursive()};
    switch (inst->GetOpcode()) {
    case IR::Opcode::FPAdd32:
        for (size_t arg = 0; arg < 2; ++arg) {
            if (SameValue(inst->Arg(arg), attribute) && checker.IsUniform(inst->Arg(1 - arg))) {
                return resolved;
            }
        }
        break;
    case IR::Opcode::FPFma32:
        if (SameValue(inst->Arg(2), attribute) && checker.IsUniform(inst->Arg(0)) &&
            checker.IsUniform(inst->Arg(1))) {
            return resolved;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

IR::Attribute GenericAttribute(size_t index, size_t component) {
    return IR::Attribute::Generic0X + (index * 4 + component);
}

IR::Attribute PositionAttribute(size_t component) {
    return IR::Attribute::PositionX + component;
}

struct Candidate {
    size_t index{};
    std::array<bool, 4> has_offset{};
};
} // Anonymous namespace

void PositionVaryingPass(IR::Program& vertex, IR::Program& fragment) {
    Info& vs_info{vertex.info};
    Info& fs_info{fragment.info};
    if (vs_info.stores_indexed_attributes || fs_info.loads_indexed_attributes ||
        vs_info.stores[IR::Attribute::ViewportIndex] || vs_info.uses_render_area ||
        fs_info.uses_render_area) {
        return;
    }
    // Collect the stores of the vertex shader
    std::unordered_map<IR::Attribute, Store> stores;
    for (IR::Block* const block : vertex.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            Store& store{stores[inst.Arg(0).Attribute()]};
            store.block = block;
            store.inst = &inst;
            store.value = inst.Arg(1);
            ++store.count;
        }
    }
    const auto unique_store = [&](IR::Attribute attribute) -> const Store* {
        const auto it = stores.find(attribute);
        return it != stores.end() && it->second.count == 1 ? &it->second : nullptr;
    };
    UniformChecker checker;
    std::vector<Candidate> candidates;
    for (size_t index = 0; index < 32; ++index) {
        if (fs_info.interpolation[index] != Interpolation::Smooth) {
            continue;
        }
        bool any_load = false;
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            any_load |= fs_info.loads.Generic(index, component);
        }
        if (!any_load) {
            continue;
        }
        Candidate candidate{.index = index};
        bool matches = true;
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            const Store* const attribute{unique_store(GenericAttribute(index, component))};
            const Store* const position{unique_store(PositionAttribute(component))};
            if (!attribute || !position || attribute->block != position->block) {
                matches = false;
                break;
            }
            const std::optional<IR::Value> offset{
                PositionOffset(checker, attribute->value, position->value)};
            if (!offset) {
                matches = false;
                break;
            }
            candidate.has_offset[component] = !offset->IsEmpty();
        }
        if (matches) {
            candidates.push_back(candidate);
        }
    }
    if (candidates.empty()) {
        return;
    }
    // Offsets are transported in a free flat varying, one per candidate needing it
    std::array<std::optional<size_t>, 32> offset_slot{};
    size_t next_free = 0;
    const auto find_free_slot = [&]() -> std::optional<size_t> {
        for (; next_free < 32; ++next_free) {
            if (!vs_info.stores.Generic(next_free) && !fs_info.loads.Generic(next_free)) {
                return next_free++;
            }
        }
        return std::nullopt;
    };
    std::erase_if(candidates, [&](const Candidate& candidate) {
        const bool needs_offset = std::ranges::any_of(candidate.has_offset, [](bool v) { return v; });
        if (!needs_offset) {
            return false;
        }
        const std::optional<size_t> slot{find_free_slot()};
        if (!slot) {
            return true;
        }
        offset_slot[candidate.index] = *slot;
        // Store position - attribute in the free slot, right before the last of the stores
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            const Store& attribute{*unique_store(GenericAttribute(candidate.index, component))};
            const Store& position{*unique_store(PositionAttribute(component))};
            IR::Inst* const last{[&] {
                for (IR::Inst& inst : attribute.block->Instructions()) {
                    if (&inst == attribute.inst) {
                        return position.inst;
                    }
                    if (&inst == position.inst) {
                        return attribute.inst;
                    }
                }
                return position.inst;
            }()};
            IR::IREmitter ir{*attribute.block, IR::Block::InstructionList::s_iterator_to(*last)};
            const IR::F32 offset{
                candidate.has_offset[component]
                    ? IR::F32{ir.FPAdd(IR::F32{position.value}, ir.FPNeg(IR::F32{attribute.value}))}
                    : ir.Imm32(0.0f)};
            ir.SetAttribute(GenericAttribute(*slot, component), offset, ir.Imm32(0));
            vs_info.stores.Set(GenericAttribute(*slot, component));
            fs_info.loads.Set(GenericAttribute(*slot, component));
        }
        fs_info.interpolation[*slot] = Interpolation::Flat;
        return false;
    });
    if (candidates.empty()) {
        return;
    }
    // Rebuild the fragment shader reads from gl_FragCoord and the host viewport
    std::array<std::array<bool, 4>, 32> rebuild{};
    for (const Candidate& candidate : candidates) {
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            rebuild[candidate.index][component] = true;
        }
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> reads;
    for (IR::Block* const block : fragment.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::GetAttribute) {
                continue;
            }
            const IR::Attribute attribute{inst.Arg(0).Attribute()};
            if (!IR::IsGeneric(attribute)) {
                continue;
            }
            const size_t index{IR::GenericAttributeIndex(attribute)};
            const size_t component{IR::GenericAttributeElement(attribute)};
            if (rebuild[index][component]) {
                reads.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : reads) {
        const IR::Attribute attribute{inst->Arg(0).Attribute()};
        const size_t index{IR::GenericAttributeIndex(attribute)};
        const size_t component{IR::GenericAttributeElement(attribute)};
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::F32 w{ir.FPRecip(ir.GetAttribute(IR::Attribute::PositionW))};
        IR::F32 value{w};
        if (component != 3) {
            // ndc = (frag_coord - viewport.offset) / viewport.extent * 2 - 1
            const IR::F32 frag_coord{ir.GetAttribute(PositionAttribute(component))};
            const IR::F32 origin{ir.HostViewport(component)};
            const IR::F32 extent{ir.HostViewport(component + 2)};
            const IR::F32 relative{
                ir.FPMul(ir.FPAdd(frag_coord, ir.FPNeg(origin)), ir.FPRecip(extent))};
            const IR::F32 ndc{ir.FPFma(relative, ir.Imm32(2.0f), ir.Imm32(-1.0f))};
            value = IR::F32{ir.FPMul(ndc, w)};
        }
        if (const std::optional<size_t> slot{offset_slot[index]}) {
            const IR::F32 offset{ir.GetAttribute(GenericAttribute(*slot, component))};
            value = IR::F32{ir.FPAdd(value, ir.FPNeg(offset))};
        }
        inst->ReplaceUsesWith(value);
    }
    fs_info.loads.Set(IR::Attribute::PositionX);
    fs_info.loads.Set(IR::Attribute::PositionY);
    fs_info.loads.Set(IR::Attribute::PositionW);
    fs_info.uses_host_viewport = true;
    LOG_DEBUG(Shader, "Rebuilding {} position varyings from the fragment coordinate",
              candidates.size());
}

// Generalization of the above for hosts supporting SPV_KHR_fragment_shader_barycentric: the
// fragment shader reads the varyings of the three vertices and interpolates them with
// barycentrics computed from the clip space positions of the vertices and gl_FragCoord. Unlike the
// hardware barycentrics of clipped primitives these are exact for any varying.
void BarycentricVaryingPass(IR::Program& vertex, IR::Program& fragment) {
    Info& vs_info{vertex.info};
    Info& fs_info{fragment.info};
    if (vs_info.stores_indexed_attributes || fs_info.loads_indexed_attributes ||
        vs_info.stores[IR::Attribute::ViewportIndex] || vs_info.uses_render_area ||
        fs_info.uses_render_area || fragment.blocks.empty()) {
        return;
    }
    // Smooth varyings read by the fragment shader only as floats
    std::array<bool, 32> convert{};
    bool any_convert = false;
    for (size_t index = 0; index < 32; ++index) {
        convert[index] = fs_info.interpolation[index] == Interpolation::Smooth &&
                         fs_info.loads.Generic(index) && vs_info.stores.Generic(index);
        any_convert |= convert[index];
    }
    if (!any_convert) {
        return;
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> reads;
    for (IR::Block* const block : fragment.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode opcode{inst.GetOpcode()};
            if (opcode != IR::Opcode::GetAttribute && opcode != IR::Opcode::GetAttributeU32) {
                continue;
            }
            const IR::Attribute attribute{inst.Arg(0).Attribute()};
            if (!IR::IsGeneric(attribute)) {
                continue;
            }
            const size_t index{IR::GenericAttributeIndex(attribute)};
            if (!convert[index]) {
                continue;
            }
            if (opcode == IR::Opcode::GetAttributeU32) {
                convert[index] = false;
                continue;
            }
            reads.emplace_back(block, &inst);
        }
    }
    std::erase_if(reads, [&](const auto& read) {
        return !convert[IR::GenericAttributeIndex(read.second->Arg(0).Attribute())];
    });
    if (reads.empty()) {
        return;
    }
    // Free varying for the clip space position
    std::optional<size_t> slot;
    for (size_t index = 0; index < 32; ++index) {
        if (!vs_info.stores.Generic(index) && !fs_info.loads.Generic(index)) {
            slot = index;
            break;
        }
    }
    if (!slot) {
        return;
    }
    // Vertex shader: copy every store of the position to the free varying
    std::vector<std::pair<IR::Block*, IR::Inst*>> position_stores;
    for (IR::Block* const block : vertex.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            const IR::Attribute attribute{inst.Arg(0).Attribute()};
            if (attribute == IR::Attribute::PositionX || attribute == IR::Attribute::PositionY ||
                attribute == IR::Attribute::PositionW) {
                position_stores.emplace_back(block, &inst);
            }
        }
    }
    if (position_stores.empty()) {
        return;
    }
    for (const auto& [block, inst] : position_stores) {
        const size_t component{static_cast<size_t>(inst->Arg(0).Attribute()) -
                               static_cast<size_t>(IR::Attribute::PositionX)};
        IR::IREmitter ir{*block, ++IR::Block::InstructionList::s_iterator_to(*inst)};
        ir.SetAttribute(GenericAttribute(*slot, component), IR::F32{inst->Arg(1)}, ir.Imm32(0));
    }
    for (const size_t component : RECONSTRUCTED_COMPONENTS) {
        vs_info.stores.Set(GenericAttribute(*slot, component));
        fs_info.loads.Set(GenericAttribute(*slot, component));
    }
    // Fragment shader: barycentrics at the beginning of the entry block
    IR::Block& entry{*fragment.blocks.front()};
    IR::IREmitter ir{entry, entry.begin()};
    const auto ndc = [&](size_t component) {
        const IR::F32 frag_coord{ir.GetAttribute(PositionAttribute(component))};
        const IR::F32 origin{ir.HostViewport(component)};
        const IR::F32 extent{ir.HostViewport(component + 2)};
        const IR::F32 relative{
            ir.FPMul(ir.FPAdd(frag_coord, ir.FPNeg(origin)), ir.FPRecip(extent))};
        return IR::F32{ir.FPFma(relative, ir.Imm32(2.0f), ir.Imm32(-1.0f))};
    };
    const IR::F32 qx{ndc(0)};
    const IR::F32 qy{ndc(1)};
    std::array<std::array<IR::F32, 3>, 3> v;
    for (u32 vertex_index = 0; vertex_index < 3; ++vertex_index) {
        for (size_t i = 0; i < 3; ++i) {
            v[vertex_index][i] = ir.GetAttribute(
                GenericAttribute(*slot, RECONSTRUCTED_COMPONENTS[i]), ir.Imm32(vertex_index));
        }
    }
    const auto sub = [&](const IR::F32& a, const IR::F32& b) { return IR::F32{ir.FPAdd(a, ir.FPNeg(b))}; };
    const auto mul = [&](const IR::F32& a, const IR::F32& b) { return IR::F32{ir.FPMul(a, b)}; };
    // b_i = dot(cross(v_j, v_k), (qx, qy, 1))
    const auto edge = [&](const std::array<IR::F32, 3>& a, const std::array<IR::F32, 3>& b) {
        const IR::F32 cx{sub(mul(a[1], b[2]), mul(a[2], b[1]))};
        const IR::F32 cy{sub(mul(a[2], b[0]), mul(a[0], b[2]))};
        const IR::F32 cz{sub(mul(a[0], b[1]), mul(a[1], b[0]))};
        return IR::F32{ir.FPFma(cx, qx, ir.FPFma(cy, qy, cz))};
    };
    const IR::F32 e0{edge(v[1], v[2])};
    const IR::F32 e1{edge(v[2], v[0])};
    const IR::F32 e2{edge(v[0], v[1])};
    const IR::F32 inv_sum{ir.FPRecip(ir.FPAdd(e0, ir.FPAdd(e1, e2)))};
    const std::array<IR::F32, 3> bary{mul(e0, inv_sum), mul(e1, inv_sum), mul(e2, inv_sum)};
    // Interpolate the reads as v0 + b1 * (v1 - v0) + b2 * (v2 - v0), like the hardware: a varying
    // with the same value on the three vertices gets exactly that value (b0 * v + b1 * v + b2 * v
    // does not, and Breath of the Wild truncates such a varying to build a material mask)
    for (const auto& [block, inst] : reads) {
        const IR::Attribute attribute{inst->Arg(0).Attribute()};
        IR::IREmitter read_ir{*block, IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::F32 v0{read_ir.GetAttribute(attribute, read_ir.Imm32(0))};
        IR::F32 value{v0};
        for (u32 vertex_index = 1; vertex_index < 3; ++vertex_index) {
            const IR::F32 vi{read_ir.GetAttribute(attribute, read_ir.Imm32(vertex_index))};
            value = IR::F32{read_ir.FPFma(bary[vertex_index],
                                          IR::F32{read_ir.FPAdd(vi, read_ir.FPNeg(v0))}, value)};
        }
        inst->ReplaceUsesWith(value);
    }
    for (size_t index = 0; index < 32; ++index) {
        if (convert[index]) {
            fs_info.interpolation[index] = Interpolation::PerVertex;
        }
    }
    fs_info.interpolation[*slot] = Interpolation::PerVertex;
    fs_info.loads.Set(IR::Attribute::PositionX);
    fs_info.loads.Set(IR::Attribute::PositionY);
    fs_info.uses_host_viewport = true;
}

} // namespace Shader::Optimization
