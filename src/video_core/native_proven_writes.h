// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "video_core/buffer_cache/proven_compute_writes.h"

namespace VideoCommon::NativeProvenWrites {

inline bool GateEnabled(std::string_view value) { return value == "1"; }

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PROVEN_WRITES");
        return value && GateEnabled(value);
    }();
    return enabled;
}

// This only widens write-footprint analysis. It never enables access/sync proofs.
inline bool GlobalReadsGateEnabled(bool proven_writes, std::string_view value) {
    return proven_writes && GateEnabled(value);
}

inline bool GlobalReadsEnabled() {
    if (!Enabled()) return false;
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PROVEN_WRITES_GLOBAL_READS");
        return GlobalReadsGateEnabled(true, value ? value : "");
    }();
    return enabled;
}

inline bool RangeMapGateEnabled(bool proven_writes, std::string_view value) {
    return proven_writes && GateEnabled(value);
}

inline bool RangeMapEnabled() {
    if (!Enabled()) return false;
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PROVEN_RANGE_MAP");
        return RangeMapGateEnabled(true, value ? value : "");
    }();
    return enabled;
}

inline bool SyncGateEnabled(bool proven_writes, std::string_view value) {
    return proven_writes && GateEnabled(value);
}

inline bool SyncEnabled() {
    if (!Enabled()) return false;
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PROVEN_SYNC");
        return SyncGateEnabled(true, value ? value : "");
    }();
    return enabled;
}

// Back-thread-only counters, no callback or reference to a pipeline/channel.
// The clock is read once per1024written bindings, and one aggregate log is
// emitted per10seconds. Repeated mark bytes are NOT actual download traffic.
inline void Record(const ProvenComputeWrites::Result& result) {
    if (!Enabled()) return;
    struct State {
        ProvenComputeWrites::Counters total;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    };
    static thread_local State state;
    state.total.Add(result);
    if ((state.total.calls & 1023U) != 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last < std::chrono::seconds{10}) return;
    state.last = now;
    const auto& value = state.total;
    const auto reason = [&](ProvenComputeWrites::Reason which) {
        return value.reasons[static_cast<std::size_t>(which)];
    };
    using ProvenComputeWrites::Reason;
    LOG_INFO(Render_Vulkan,
        "NX2 proven writes enabled: bindings={} proven={} full={} original_bytes={} "
        "marked_bytes={} ranges={} missing={} indirect={} invalid={} alignment={} bounds={} "
        "changed={} noncontiguous={} alias={} images={} (repeated tracking bytes, not downloads)",
        value.calls, value.proven, value.calls - value.proven, value.original_bytes,
        value.marked_bytes, value.ranges, reason(Reason::MissingProof),
        reason(Reason::IndirectOrShapeMismatch), reason(Reason::InvalidBinding),
        reason(Reason::AlignmentMismatch), reason(Reason::UnprovedBounds),
        reason(Reason::BindingChanged), reason(Reason::NonContiguous),
        reason(Reason::WritableAlias), reason(Reason::OtherImageWrites));
}
} // namespace VideoCommon::NativeProvenWrites
