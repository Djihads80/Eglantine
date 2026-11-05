// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Service::Nx2 {

struct PresentFence {
    u32 id{}, value{};
    bool operator==(const PresentFence&) const = default;
};

struct PresentReplyFence {
    u64 layer{};
    PresentFence fence;
};

inline bool PreviousPresentOnlyEnabled(std::string_view value) {
    return value == "1";
}

// Experimental scope only. A processed Guest fence is not a physical-GPU completion or proof
// that the CPU may overwrite submitted command buffers.
inline bool CanOmitCurrentPresentWait(bool enabled, u64 program_id, int frames_ahead, u64 layer,
                                     PresentFence previous, PresentFence current,
                                     const std::optional<PresentReplyFence>& last_reply) {
    constexpr u64 OotProgramId = 0x0400486024DEC000ULL;
    if (!enabled || program_id != OotProgramId || frames_ahead != 1 || !layer || !last_reply ||
        last_reply->layer != layer || last_reply->fence != previous || previous.id >= 192 ||
        current.id != previous.id) {
        return false;
    }
    const u32 distance = current.value - previous.value;
    // Host1x readiness still uses unsigned >=; do not opt into overlap across wrap.
    return current.value > previous.value && distance < 0x80000000U;
}

// Preserve the previous wait even when the diagnostic omits the second wait. Failure remains
// failure; callers must reject the present rather than publishing a successful release reply.
template <typename Wait>
bool WaitForPresentFences(PresentFence previous, PresentFence current, bool wait_current,
                          Wait&& wait) {
    const bool previous_ready = wait(previous);
    if (wait_current) {
        return wait(current) && previous_ready;
    }
    return previous_ready;
}

// Count actual entry-to-entry intervals: startup 600 presents contain 599 intervals;
// subsequent windows preserve last_present and therefore contain 600 intervals.
struct PresentIntervalAccumulator {
    long long sum_us{};
    u32 intervals{};
    void Add(long long elapsed_us) {
        sum_us += elapsed_us;
        ++intervals;
    }
    long long MeanMicroseconds() const {
        return intervals ? sum_us / intervals : 0;
    }
};

} // namespace Service::Nx2
