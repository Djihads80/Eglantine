// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <climits>
#include <vector>

#include "common/common_types.h"

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct StagingBufferRef {
    VkBuffer buffer;
    VkDeviceAddress device_address;
    VkDeviceSize offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 size_class;
    u64 index;
};

class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;

    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    void TickFrame();

private:
    struct StreamBufferCommit {
        size_t upper_bound;
        u64 tick;
    };

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 size_class;
        u64 index;
        u64 tick = 0;
        u64 frame = 0; ///< Last frame a request got this buffer.
        bool deferred{};

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .size_class = size_class,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        size_t iterate_index = 0;
    };

    /// Size classes up to this power of two are powers of two; above it there are four per
    /// doubling.
    static constexpr u32 FINE_SIZE_CLASSES_FROM_LOG2 = 20;
    static constexpr size_t NUM_LEVELS =
        FINE_SIZE_CLASSES_FROM_LOG2 + (sizeof(size_t) * CHAR_BIT - FINE_SIZE_CLASSES_FROM_LOG2) * 4 + 1;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    static u32 SizeClass(size_t size);
    static size_t ClassSize(u32 size_class);

    StagingBufferRef GetStreamBuffer(size_t size);

    bool AreRegionsActive(size_t region_begin, size_t region_end) const;

    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);

    std::optional<StagingBufferRef> TryGetReservedBuffer(u32 size_class, MemoryUsage usage,
                                                         bool deferred);

    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);

    StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(StagingBuffersCache& cache, size_t& deletions_left);
    size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    vk::Buffer stream_buffer;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;
    VkDeviceSize region_size;

    size_t iterator = 0;
    size_t used_iterator = 0;
    size_t free_iterator = 0;
    std::array<u64, NUM_SYNCS> sync_ticks{};

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    u64 frame = 0;
    u64 buffer_index = 0;
    u64 unique_ids{};
};

} // namespace Vulkan
