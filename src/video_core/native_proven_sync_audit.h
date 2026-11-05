// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <string_view>
#include "common/logging.h"
#include "video_core/buffer_cache/proven_compute_sync.h"
#include "video_core/native_proven_writes.h"

namespace VideoCommon::NativeProvenSyncAudit {
inline bool GateEnabled(bool sync_enabled, std::string_view value) {
    return sync_enabled && value == "1";
}
inline bool Enabled() {
    if (!NativeProvenWrites::SyncEnabled()) return false;
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PROVEN_SYNC_AUDIT");
        return GateEnabled(true, value ? std::string_view{value} : std::string_view{});
    }();
    return enabled;
}
struct Counters {
    u64 calls{}, full{}, narrow{};
    u64 reference_full_span_bytes{}, requested_span_bytes{}, narrow_requested_span_bytes{};

    // These are ranges requested from SynchronizeBuffer, including repeated
    // requests. Dirty-page expansion/clean skips mean they are NOT transfer bytes.
    bool Observe(ProvenComputeSync::Outcome outcome, u32 binding_bytes, u64 requested_bytes) {
        ++calls;
        reference_full_span_bytes += binding_bytes;
        requested_span_bytes += requested_bytes;
        if (outcome == ProvenComputeSync::Outcome::Narrow) {
            ++narrow;
            narrow_requested_span_bytes += requested_bytes;
        } else {
            ++full;
        }
        return calls == 1024 || (calls & 65535) == 0;
    }
};
inline void Record(ProvenComputeSync::Outcome outcome, u32 binding_bytes, u64 requested_bytes) {
    if (!Enabled()) return;
    thread_local Counters counters;
    if (!counters.Observe(outcome, binding_bytes, requested_bytes)) return;
    LOG_INFO(HW_GPU,
        "ProvenSyncAudit calls={} full={} narrow={} reference_full_span_bytes={} "
        "requested_span_bytes={} narrow_requested_span_bytes={} (request spans, not transfer bytes)",
        counters.calls, counters.full, counters.narrow, counters.reference_full_span_bytes,
        counters.requested_span_bytes, counters.narrow_requested_span_bytes);
}
} // namespace VideoCommon::NativeProvenSyncAudit
