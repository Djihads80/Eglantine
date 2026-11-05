// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <utility>
#include <vector>

#include "video_core/nx2_command_writer_trace.h"

namespace VideoCommon::CommandWriterTrace {

// Captured aliases/context are values. No pointer is retained for dereferencing after the store.
class CpuWriteObservation {
public:
    void Capture(const Registry& registry, u64 owner, Range device_range, u64 virtual_address,
                 u64 host_pointer, CpuContext context) {
        registry.Device(owner, device_range, [&](const Watch& watch, Range overlap) {
            for (const auto& event : events) {
                if (event.watch_id == watch.id && event.range.address == device_range.address &&
                    event.range.size == device_range.size) {
                    return;
                }
            }
            events.push_back({.watch_id = watch.id,
                              .kind = Kind::CpuCompletedWrite,
                              .phase = Phase::After,
                              .domain = Domain::Device,
                              .extent = Extent::ExactBytes,
                              .range = device_range,
                              .overlap = overlap,
                              .aux0 = virtual_address,
                              .aux1 = host_pointer,
                              .cpu = context});
        });
    }

    bool Empty() const { return events.empty(); }

    template <typename Append>
    void Complete(bool committed, u64 ns, u64 present, Append&& append) {
        auto completed = std::exchange(events, {});
        if (!committed) {
            return;
        }
        for (auto& event : completed) {
            event.ns = ns;
            event.present = present;
            append(event);
        }
    }

private:
    // Production captures at most two physical pages for one <=16-byte scalar/CAS. Each of
    // the bounded registry watches can match at most once per page; duplicate aliases dedupe.
    std::vector<Event> events;
};

template <typename Prepare, typename Store, typename Publish>
bool ObserveCpuWrite(bool active, Prepare&& prepare, Store&& store, Publish&& publish) {
    if (!active) {
        return store();
    }
    auto observation = prepare();
    const bool committed = store();
    publish(observation, committed);
    return committed;
}

inline void CaptureCpuWriteRange(CpuWriteObservation& observation, u64 owner, Range device_range,
                                u64 virtual_address, u64 host_pointer) {
    if (!Active()) {
        return;
    }
    auto& state = GetState();
    const u64 low = state.low.load(std::memory_order_acquire);
    const u64 high = state.high.load(std::memory_order_acquire);
    if (high <= low || !Intersect(device_range, {low, high - low})) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    if (!state.dumped) {
        observation.Capture(state.registry, owner, device_range, virtual_address, host_pointer,
                            cpu_context);
    }
}

inline void PublishCpuWrite(CpuWriteObservation& observation, bool committed) {
    if (!committed || observation.Empty() || !Active()) {
        return;
    }
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.dumped) {
        observation.Complete(true, Now(), Core::g_nx2_present_count.load(std::memory_order_relaxed),
                             [&](const Event& event) { state.events.Append(event); });
    }
}

} // namespace VideoCommon::CommandWriterTrace
