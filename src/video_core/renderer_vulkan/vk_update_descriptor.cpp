// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <variant>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                             bool supports_descriptor_buffer_)
    : device{device_}, frame_payload_size{frame_payload_size_},
      max_frame_payload_size{frame_payload_size_ * MAX_PAYLOAD_GROWTH},
      supports_descriptor_buffer{supports_descriptor_buffer_},
      payload(std::make_unique_for_overwrite<DescriptorUpdateEntry[]>(frame_payload_size_ * FRAMES_IN_FLIGHT))
{
    payload_start = payload.get();
    payload_cursor = payload.get();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload.get() + frame_index * frame_payload_size;
    payload_cursor = payload_start;
}

void UpdateDescriptorQueue::Acquire(Scheduler& scheduler, size_t required_entries,
                                    bool use_descriptor_buffer_) {
    use_descriptor_buffer = supports_descriptor_buffer && use_descriptor_buffer_;
    static constexpr size_t DEFAULT_REQUIRED_ENTRIES = 0x400;
    const size_t reserve = required_entries > 0 ? required_entries : DEFAULT_REQUIRED_ENTRIES;
    ASSERT_MSG(reserve < frame_payload_size, "Descriptor reservation {} >= frame capacity {}",
               reserve, frame_payload_size);
    const size_t used = static_cast<size_t>(std::distance(payload_start, payload_cursor));
    if (used + reserve >= frame_payload_size) {
        // The worker reads this slice until it has run the commands recorded with it: wait for it.
        // Every recorded command has run then, so nothing points into the payload and it may grow.
        // Fortune's Weave writes over a million descriptors a frame (buffer-backed texture arrays of
        // hundreds of records per draw) and overflowed twice per frame, each time stalling the GPU
        // thread until the worker drained; a bigger slice stops the overflows.
        scheduler.WaitWorker();
        if (frame_payload_size < max_frame_payload_size) {
            frame_payload_size *= 2;
            payload = std::make_unique_for_overwrite<DescriptorUpdateEntry[]>(frame_payload_size *
                                                                               FRAMES_IN_FLIGHT);
            payload_start = payload.get() + frame_index * frame_payload_size;
            LOG_INFO(Render_Vulkan, "Descriptor payload grown to {} entries per frame (used={}, reserve={})",
                     frame_payload_size, used, reserve);
        } else {
            LOG_WARNING(Render_Vulkan, "Payload overflow (used={}, reserve={}, capacity={})",
                        used, reserve, frame_payload_size);
        }
        payload_cursor = payload_start;
    }
    upload_start = payload_cursor;
}

} // namespace Vulkan
