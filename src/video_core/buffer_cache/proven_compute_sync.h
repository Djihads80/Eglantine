// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include "common/common_types.h"
#include "shader_recompiler/storage_write_bounds.h"

namespace VideoCommon::ProvenComputeSync {
using Shader::StorageWriteBounds::ByteRange;
using Shader::StorageWriteBounds::DescriptorAccessProof;
using Shader::StorageWriteBounds::Dispatch;
constexpr u64 PageSize = 4096;
constexpr std::size_t MaxBindings = 32;
enum class Coverage { None, FullMapping, ReadWriteRanges };
enum class Outcome { Full, Narrow };
struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u32 size{};
    bool enabled{};
    Coverage coverage{Coverage::None};
};
inline bool Valid(const Binding& binding) {
    constexpr u64 Limit = u64{1} << 36;
    return binding.enabled && binding.device_addr != 0 && binding.size != 0 &&
           binding.device_addr < Limit && binding.size <= Limit - binding.device_addr &&
           binding.gpu_addr != 0 && binding.size <= std::numeric_limits<u64>::max() - binding.gpu_addr;
}

class Plan {
public:
    void Reset() {
        if (!used) return;
        for (auto& entry : entries) entry = {};
        used = false;
    }
    void Prepare(std::size_t index, const Binding& binding, const DescriptorAccessProof& access,
                 const Dispatch& dispatch, u32 alignment) {
        if (index >= entries.size()) return;
        used = true;
        auto& entry = entries[index];
        entry = {};
        entry.binding = binding;
        const auto& proof = access.bounds;
        if (!Valid(binding) || !proof.complete || !dispatch.direct ||
            proof.binding_alignment != alignment) return;
        entry.ranges = Shader::StorageWriteBounds::Evaluate(
            proof.nodes, proof.stores, dispatch, 0, binding.size, true);
    }
    bool HasCandidate() const {
        return used && std::ranges::any_of(entries, [](const auto& entry) { return entry.ranges.has_value(); });
    }
    // No dirty bit may be consumed until preflight has completed for ALL bindings.
    // validate_mapping certifies CURRENT GPU->DAddr identity and every byte of linearity.
    // host_bounds covers the full pages emitted by MemoryTracker, not a clipped upload.
    template <typename ValidateMapping, typename HostBounds>
    Coverage Preflight(std::size_t index, const Binding& binding, bool host_eligible,
                       ValidateMapping&& validate_mapping, HostBounds&& host_bounds) const {
        if (!host_eligible || !Valid(binding)) return Coverage::None;
        if (!Matches(index, binding)) {
            return validate_mapping(binding.gpu_addr, binding.device_addr, binding.size)
                ? Coverage::FullMapping : Coverage::None;
        }
        for (const ByteRange& range : *entries[index].ranges) {
            if (range.begin >= range.end || range.end > binding.size) return Coverage::None;
            const DAddr first = binding.device_addr + range.begin;
            const DAddr end = binding.device_addr + range.end;
            const DAddr page_first = first & ~(PageSize - 1);
            const DAddr page_end = (end + PageSize - 1) & ~(PageSize - 1);
            if (!host_bounds(page_first, page_end - page_first) ||
                !validate_mapping(binding.gpu_addr + range.begin, first,
                                  static_cast<u32>(range.end - range.begin))) {
                return Coverage::None;
            }
        }
        return Coverage::ReadWriteRanges;
    }
    template <typename Synchronize>
    Outcome Apply(std::size_t index, std::span<const Binding> current, bool image_accesses,
                  Synchronize&& synchronize) const {
        if (index >= current.size()) return Outcome::Full;
        const auto& binding = current[index];
        const bool narrow = [&] {
            if (image_accesses || !Matches(index, binding)) return false;
            for (std::size_t other = 0; other < current.size(); ++other) {
                const auto& peer = current[other];
                if (!peer.enabled) continue;
                if (!Valid(peer) ||
                    (peer.coverage != Coverage::FullMapping &&
                     !(peer.coverage == Coverage::ReadWriteRanges && Matches(other, peer)))) return false;
                if (other == index) continue;
                // Include read-only peers. Entire physical spans deliberately overestimate access.
                if (binding.device_addr < peer.device_addr + peer.size &&
                    peer.device_addr < binding.device_addr + binding.size) return false;
            }
            return true;
        }();
        if (!narrow) {
            synchronize(binding.device_addr, binding.size);
            return Outcome::Full;
        }
        for (const ByteRange& range : *entries[index].ranges) {
            // SynchronizeBuffer owns page expansion and GPU-written subtraction. Never clip
            // its callbacks after they have consumed a page's dirty bit.
            synchronize(binding.device_addr + range.begin, static_cast<u32>(range.end - range.begin));
        }
        return Outcome::Narrow;
    }
private:
    bool Matches(std::size_t index, const Binding& binding) const {
        if (index >= entries.size()) return false;
        const auto& entry = entries[index];
        return entry.ranges.has_value() && Valid(binding) &&
               entry.binding.device_addr == binding.device_addr && entry.binding.gpu_addr == binding.gpu_addr &&
               entry.binding.size == binding.size;
    }
    struct Entry {
        Binding binding;
        std::optional<std::vector<ByteRange>> ranges;
    };
    std::array<Entry, MaxBindings> entries;
    bool used{};
};
} // namespace VideoCommon::ProvenComputeSync
