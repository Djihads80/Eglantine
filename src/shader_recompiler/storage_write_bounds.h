// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Shader::StorageWriteBounds {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// This bytecode describes offsets relative to ONE proven original guest base pointer.
// The producer must account for EVERY store/atomic to that descriptor and must reject
// unresolved globals, cyclic phi/loop expressions, pointer chasing, and unsupported ops.
// No guest shader bytes, pointer constants, or buffer contents are stored here.
enum class Op { Constant, Workgroup, LocalInvocation, AlignmentBias, AddU32, MulU32, ShiftLeftU32,
                SelectUnion, Unknown, BitFieldExtractU32 };

struct Node {
    Op op{Op::Unknown};
    u32 a{}; // Constant value / axis / earlier node index.
    u32 b{}; // Earlier node index; BitFieldExtract packs (offset << 8) | width.
};

struct Store {
    u32 offset_node{};
    u32 bytes{}; // Initial supported subset: 32/64/128-bit stores/atomics.
    u32 address_alignment{1}; // Include SPIR-V's element-index rounding, not just guest bytes.
};

// Host metadata only. Every store/atomic must be accounted for before complete=true.
struct DescriptorProof {
    u32 cbuf_index{};
    u32 cbuf_offset{};
    u32 binding_alignment{};
    bool complete{};
    std::vector<Node> nodes;
    std::vector<Store> stores;
};

// Separate type: a write-only proof cannot authorize skipping uploads of reads.
// The contained nodes/stores describe ALL storage accesses, not only writes.
struct DescriptorAccessProof {
    DescriptorProof bounds;
};

struct Dispatch {
    std::array<u32, 3> groups{}; // Exact direct dispatch counts submitted to Vulkan.
    std::array<u32, 3> local{};  // Compiled pipeline's IR::Program::workgroup_size.
    bool direct{};
};

struct ByteRange {
    u64 begin{};
    u64 end{}; // Exclusive, relative to the unchanged aligned host binding.
    auto operator<=>(const ByteRange&) const = default;
};

struct Interval {
    u32 low{};
    u32 high{}; // Inclusive.
};

// nullopt requires the EXISTING full-bound MarkWrittenBuffer path. A present empty
// vector means a proven no-invocation direct dispatch; it is not an unknown result.
// This function NEVER shrinks bindings, clears GPU-modified state, or removes downloads.
inline std::optional<std::vector<ByteRange>> Evaluate(
    std::span<const Node> nodes, std::span<const Store> stores, const Dispatch& dispatch,
    u64 base_alignment_bias, u64 bound_size, bool complete_write_proof) {
    constexpr u64 Max = std::numeric_limits<u32>::max();
    constexpr std::size_t MaxNodes = 256;
    constexpr std::size_t MaxStores = 64;
    if (!complete_write_proof || !dispatch.direct || nodes.size() > MaxNodes ||
        stores.size() > MaxStores || bound_size == 0 || base_alignment_bias >= bound_size ||
        std::ranges::find(dispatch.local, 0U) != dispatch.local.end()) {
        return std::nullopt;
    }
    if (std::ranges::find(dispatch.groups, 0U) != dispatch.groups.end()) {
        return std::vector<ByteRange>{};
    }
    std::array<std::optional<Interval>, MaxNodes> values{};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const Node& node = nodes[index];
        if (node.op == Op::Constant) {
            values[index] = Interval{node.a, node.a};
            continue;
        }
        if (node.op == Op::AlignmentBias) {
            if (node.a != 0 && (node.a & (node.a - 1)) == 0) {
                values[index] = Interval{0, node.a - 1};
            }
            continue;
        }
        if (node.op == Op::Workgroup || node.op == Op::LocalInvocation) {
            if (node.a < 3) {
                const auto& shape = node.op == Op::Workgroup ? dispatch.groups : dispatch.local;
                values[index] = Interval{0, shape[node.a] - 1};
            }
            continue;
        }
        if (node.op == Op::BitFieldExtractU32) {
            const u32 offset = (node.b >> 8) & 0xffU;
            const u32 width = node.b & 0xffU;
            if (node.a >= index || !values[node.a] || node.b > 0xffffU ||
                offset > 32 || width > 32 - offset) {
                continue;
            }
            // Validate the source before width zero: extraction cannot hide a
            // previously unknown/wrapping address operation. Use U64 for width32.
            if (width == 0) {
                values[index] = Interval{0, 0};
                continue;
            }
            const u64 low = u64{values[node.a]->low} >> offset;
            const u64 high = u64{values[node.a]->high} >> offset;
            const u64 period = u64{1} << width;
            const u64 mask = period - 1;
            // Within one modulo bucket, extraction is monotone. Crossing a
            // boundary can reverse the endpoints, so conservatively use all bits.
            values[index] = low / period == high / period
                ? Interval{static_cast<u32>(low & mask), static_cast<u32>(high & mask)}
                : Interval{0, static_cast<u32>(mask)};
            continue;
        }
        // Acyclic, topologically ordered graph; no forward reads or recursive evaluation.
        if (node.a >= index || node.b >= index || !values[node.a] || !values[node.b]) {
            continue;
        }
        const Interval left = *values[node.a];
        const Interval right = *values[node.b];
        u64 low{};
        u64 high{};
        switch (node.op) {
        case Op::AddU32:
            low = u64{left.low} + right.low;
            high = u64{left.high} + right.high;
            break;
        case Op::MulU32:
            low = u64{left.low} * right.low;
            high = u64{left.high} * right.high;
            break;
        case Op::ShiftLeftU32:
            if (right.low != right.high || right.high >= 32) {
                continue;
            }
            low = u64{left.low} << right.low;
            high = u64{left.high} << right.high;
            break;
        case Op::SelectUnion:
            low = std::min(left.low, right.low);
            high = std::max(left.high, right.high);
            break;
        default:
            continue;
        }
        // Every intermediate U32 operation must be proved non-wrapping, not just the final sum.
        // Modular/signed patterns are deliberately unsupported in this first subset.
        if (high <= Max) {
            values[index] = Interval{static_cast<u32>(low), static_cast<u32>(high)};
        }
    }
    std::vector<ByteRange> ranges;
    ranges.reserve(stores.size());
    for (const Store& store : stores) {
        if (store.offset_node >= nodes.size() || !values[store.offset_node] ||
            (store.bytes != 4 && store.bytes != 8 && store.bytes != 16) ||
            store.address_alignment == 0 || store.address_alignment > store.bytes ||
            (store.address_alignment & (store.address_alignment - 1)) != 0) {
            return std::nullopt;
        }
        const Interval offset = *values[store.offset_node];
        // Subtraction-first guards also reject U64 addition overflow. No clipping silently
        // converts an unproved or out-of-binding access into a successful proof.
        if (offset.high > bound_size - base_alignment_bias ||
            store.bytes > bound_size - base_alignment_bias - offset.high) {
            return std::nullopt;
        }
        // Vector aliases use width-aligned element indices, scalar fallback uses 4-byte
        // indices. Rounding down by the widest possible element conservatively includes
        // both; the upper bound deliberately stays unrounded.
        const u64 rounded_low = (base_alignment_bias + offset.low) &
                                ~(u64{store.address_alignment} - 1);
        ranges.push_back({rounded_low,
                          base_alignment_bias + offset.high + store.bytes});
    }
    std::ranges::sort(ranges, {}, &ByteRange::begin);
    std::vector<ByteRange> merged;
    for (const ByteRange range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}
} // namespace Shader::StorageWriteBounds
