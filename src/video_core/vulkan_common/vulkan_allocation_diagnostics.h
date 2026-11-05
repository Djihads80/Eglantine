// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdio>
#include "video_core/nx2_command_trace.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
// Error-only observation of existing owned rings; never changes allocation or retry policy.
inline void ReportAllocationFailure(VkResult result, const char* operation, u64 size_or_count,
                                    u64 alignment, u64 flags, u64 extra) noexcept {
    if ((result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) ||
        !VideoCommon::CommandTrace::Enabled()) {
        return;
    }
    static std::atomic_flag reported;
    if (reported.test_and_set(std::memory_order_relaxed)) {
        return;
    }
    namespace Trace = VideoCommon::CommandTrace;
    const auto present = Trace::Present();
    std::fprintf(stderr,
        "NX2 allocation failure: operation=%s result=%d present=%llu in_window=%d "
        "size_or_count=%llu alignment=%llu flags=%llu extra=%llu\n",
        operation, static_cast<int>(result), static_cast<unsigned long long>(present),
        static_cast<int>(Trace::InWindow(present)), static_cast<unsigned long long>(size_or_count),
        static_cast<unsigned long long>(alignment), static_cast<unsigned long long>(flags),
        static_cast<unsigned long long>(extra));
    std::fflush(stderr);
    try {
        auto origin = Trace::current_origin;
        origin.present = present;
        Trace::Event event{.origin = origin, .a = size_or_count, .b = alignment,
                           .c = flags, .d = extra, .kind = Trace::EventKind::AllocationFailure};
        event.words[0] = static_cast<u32>(result);
        Trace::Record(event);
        Trace::Dump(operation);
    } catch (...) {
        // Snapshot/export can allocate or fail I/O. Preserve the original Vulkan exception.
        std::fprintf(stderr, "NX2 allocation failure: diagnostic export failed\n");
        std::fflush(stderr);
    }
}
} // namespace Vulkan
