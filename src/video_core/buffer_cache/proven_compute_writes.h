// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/storage_write_bounds.h"

namespace VideoCommon::ProvenComputeWrites {

constexpr std::size_t MaxBindings = 32;
using Shader::StorageWriteBounds::ByteRange;
using Shader::StorageWriteBounds::DescriptorProof;
using Shader::StorageWriteBounds::Dispatch;

enum class Coverage { None, FullMapping, ProvenWriteRanges };

struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u32 size{};
    bool written{};
    Coverage coverage{Coverage::None}; // ProvenWriteRanges does not certify reads or gaps.
};

enum class Reason : u32 {
    Proven,
    MissingProof,
    IndirectOrShapeMismatch,
    InvalidBinding,
    AlignmentMismatch,
    UnprovedBounds,
    BindingChanged,
    NonContiguous,
    WritableAlias,
    OtherImageWrites,
    Count,
};

struct Result {
    Reason reason{Reason::MissingProof};
    u64 original_bytes{};
    u64 marked_bytes{};
    u64 ranges{};
};

inline bool Valid(const Binding& binding) {
    // DAddr is a 36-bit device address throughout this buffer cache. Subtraction
    // guards reject wrap/invalid ranges without forming an overflowing end.
    constexpr u64 Limit = u64{1} << 36;
    return binding.device_addr != 0 && binding.size != 0 && binding.device_addr < Limit &&
           binding.size <= Limit - binding.device_addr && binding.gpu_addr != 0 &&
           binding.size <= std::numeric_limits<u64>::max() - binding.gpu_addr;
}

class Plan {
public:
    void Reset() {
        if (!used) return;
        for (auto& entry : entries) entry = {};
        used = false;
    }

    void Prepare(std::size_t index, const Binding& binding, const DescriptorProof* proof,
                 const Dispatch& dispatch, u32 actual_alignment) {
        if (index >= entries.size()) return;
        used = true;
        Entry& entry = entries[index];
        entry = {};
        entry.binding = binding;
        if (!proof || !proof->complete) return;
        if (!dispatch.direct) {
            entry.reason = Reason::IndirectOrShapeMismatch;
            return;
        }
        if (!Valid(binding) || !binding.written) {
            entry.reason = Reason::InvalidBinding;
            return;
        }
        if (proof->binding_alignment != actual_alignment) {
            entry.reason = Reason::AlignmentMismatch;
            return;
        }
        // Alias/segmentation is checked only at Apply, after all bindings and
        // ResolveMultiRangeStorage are final. No pipeline pointer survives here.
        entry.ranges = Shader::StorageWriteBounds::Evaluate(
            proof->nodes, proof->stores, dispatch, 0, binding.size, proof->complete);
        entry.reason = entry.ranges ? Reason::Proven : Reason::UnprovedBounds;
    }

    bool HasCandidate() const {
        if (!used) return false;
        return std::ranges::any_of(entries, [](const auto& entry) { return entry.ranges.has_value(); });
    }

    // Validate only current mappings. The callback must certify BOTH the expected
    // starting DAddr and linearity of every byte; it must not reuse a prior walk.
    // host_eligible excludes null, sparse, gathered and out-of-bounds host buffers.
    template <typename ValidateRange>
    Coverage ValidateMapping(std::size_t index, const Binding& binding, bool host_eligible,
                             bool use_proven_ranges, ValidateRange&& validate_range) const {
        if (!host_eligible || !binding.written || !Valid(binding)) return Coverage::None;
        if (use_proven_ranges && MatchesPreparedProof(index, binding)) {
            for (const ByteRange& range : *entries[index].ranges) {
                if (range.begin >= range.end || range.end > binding.size ||
                    !validate_range(binding.gpu_addr + range.begin,
                                    binding.device_addr + range.begin,
                                    static_cast<u32>(range.end - range.begin))) {
                    return Coverage::None;
                }
            }
            // An empty result is possible only after Evaluate proved no direct
            // invocations; unlike missing proof, it needs no mapping reads.
            return Coverage::ProvenWriteRanges;
        }
        return validate_range(binding.gpu_addr, binding.device_addr, binding.size)
            ? Coverage::FullMapping : Coverage::None;
    }

    template <typename MarkWritten>
    Result Apply(std::size_t index, std::span<const Binding> current, bool other_image_writes,
                 MarkWritten&& mark_written) const {
        if (index >= current.size()) return {Reason::InvalidBinding};
        const Binding& binding = current[index];
        Reason reason = index < entries.size() ? entries[index].reason : Reason::MissingProof;
        if (reason == Reason::Proven) {
            const Entry& entry = entries[index];
            if (entry.binding.device_addr != binding.device_addr ||
                entry.binding.gpu_addr != binding.gpu_addr || entry.binding.size != binding.size ||
                !binding.written || !Valid(binding)) {
                reason = Reason::BindingChanged;
            } else if (other_image_writes) {
                reason = Reason::OtherImageWrites;
            } else {
                for (std::size_t other = 0; other < current.size(); ++other) {
                    if (!current[other].written) continue;
                    if (!Valid(current[other]) ||
                        (current[other].coverage != Coverage::FullMapping &&
                         !(current[other].coverage == Coverage::ProvenWriteRanges &&
                           MatchesPreparedProof(other, current[other])))) {
                        reason = Reason::NonContiguous;
                        break;
                    }
                    if (other == index) continue;
                    // GPU virtual ranges may be disjoint while sharing physical
                    // DAddr bytes. Compare the entire writable physical bindings.
                    if (binding.device_addr < current[other].device_addr + current[other].size &&
                        current[other].device_addr < binding.device_addr + binding.size) {
                        reason = Reason::WritableAlias;
                        break;
                    }
                }
            }
        }
        Result result{reason, binding.size, 0, 0};
        if (reason != Reason::Proven) {
            mark_written(binding.device_addr, binding.size);
            result.marked_bytes = binding.size;
            result.ranges = 1;
            return result;
        }
        for (const ByteRange& range : *entries[index].ranges) {
            const u32 size = static_cast<u32>(range.end - range.begin);
            mark_written(binding.device_addr + range.begin, size);
            result.marked_bytes += size;
            ++result.ranges;
        }
        return result;
    }

private:
    bool MatchesPreparedProof(std::size_t index, const Binding& binding) const {
        if (index >= entries.size()) return false;
        const Entry& entry = entries[index];
        return entry.reason == Reason::Proven && entry.ranges.has_value() &&
               entry.binding.device_addr == binding.device_addr &&
               entry.binding.gpu_addr == binding.gpu_addr && entry.binding.size == binding.size &&
               binding.written && Valid(binding);
    }

    struct Entry {
        Binding binding;
        Reason reason{Reason::MissingProof};
        std::optional<std::vector<ByteRange>> ranges;
    };
    std::array<Entry, MaxBindings> entries;
    bool used{};
};

struct Counters {
    u64 calls{}, proven{}, original_bytes{}, marked_bytes{}, ranges{};
    std::array<u64, static_cast<std::size_t>(Reason::Count)> reasons{};
    void Add(const Result& result) {
        ++calls;
        proven += result.reason == Reason::Proven;
        original_bytes += result.original_bytes;
        marked_bytes += result.marked_bytes;
        ranges += result.ranges;
        ++reasons[static_cast<std::size_t>(result.reason)];
    }
};
} // namespace VideoCommon::ProvenComputeWrites
