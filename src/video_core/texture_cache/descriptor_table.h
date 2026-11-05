// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/assert.h"
#include "core/memory.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

template <typename T, typename Id = u32>
class DescriptorTable {
public:
    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) noexcept {
        bool ret = !(current_gpu_addr == gpu_addr && current_limit == limit);
        if (ret) {
            Refresh(gpu_addr, limit);
        }
        return ret;
    }

    void Invalidate() noexcept {
        // Cleared on the next read: the texture cache invalidates every channel's tables for
        // each image it deletes, dozens of times a frame while its collector runs.
        invalidated = true;
    }

    template <class MemoryManager>
    [[nodiscard]] std::pair<T, bool> Read(const MemoryManager& gpu_memory, u32 index) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        if (invalidated) {
            ClearReadDescriptors();
            invalidated = false;
        }
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(T);
        const u64 page_offset = gpu_addr & Core::Memory::YUZU_PAGEMASK;
        std::pair<T, bool> result;
        // This runs for every texture and sampler of every draw. An entry within one page is
        // contiguous in host memory, so cache the host page pointer across descriptors in the
        // same 4 KiB page while the GPU page table mapping serial is unchanged.
        if (page_offset + sizeof(T) <= Core::Memory::YUZU_PAGESIZE) [[likely]] {
            if constexpr (requires {
                              gpu_memory.GetID();
                              gpu_memory.GetMapSerial();
                          }) {
                const GPUVAddr page_addr = gpu_addr - page_offset;
                const size_t memory_id = gpu_memory.GetID();
                const u64 map_serial = gpu_memory.GetMapSerial();
                auto& cache_entry =
                    page_cache[(page_addr >> Core::Memory::YUZU_PAGEBITS) & (PageCacheSize - 1)];
                const u8* page_ptr = nullptr;
                if (cache_entry.page_addr == page_addr && cache_entry.memory_id == memory_id &&
                    cache_entry.map_serial == map_serial && cache_entry.host_ptr != nullptr)
                    [[likely]] {
                    page_ptr = cache_entry.host_ptr;
                } else if (const u8* const resolved = gpu_memory.GetPointer(page_addr)) {
                    cache_entry.page_addr = page_addr;
                    cache_entry.memory_id = memory_id;
                    cache_entry.map_serial = map_serial;
                    cache_entry.host_ptr = resolved;
                    page_ptr = resolved;
                }
                if (page_ptr != nullptr) [[likely]] {
                    std::memcpy(std::addressof(result.first), page_ptr + page_offset, sizeof(T));
                } else {
                    gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
                }
            } else if (const u8* const host_ptr = gpu_memory.GetPointer(gpu_addr)) {
                std::memcpy(std::addressof(result.first), host_ptr, sizeof(T));
            } else {
                gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
            }
        } else {
            gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
        }
        if ((read_descriptors[index / 64] & (1ULL << (index % 64))) != 0) {
            result.second = result.first != descriptors[index];
        } else {
            read_descriptors[index / 64] |= 1ULL << (index % 64);
            result.second = true;
        }
        if (result.second) {
            descriptors[index] = result.first;
        }
        return result;
    }

    void Refresh(GPUVAddr gpu_addr, u32 limit) noexcept {
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        // Mario Brothership reallocates a lot of times, so use aggressive pre-alloc sizes
        // std::vector<T> by default uses quadratic growth, but that isn't even enough to satisfy brothership
        const size_t num_descriptors = ((limit + 0x80000) & (~0x7ffff)) + 1;
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U));
        ClearReadDescriptors();
        invalidated = false;
        //
        descriptors.resize(num_descriptors);
        bound_ids.resize(num_descriptors, Id{0});
    }

    std::vector<u64> read_descriptors;
    std::vector<T> descriptors;
    std::vector<Id> bound_ids;
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    bool invalidated{}; ///< read_descriptors is to be cleared before the next read

private:
    static constexpr size_t PageCacheSize = 16;
    struct PageCacheEntry {
        GPUVAddr page_addr{~GPUVAddr{0}};
        size_t memory_id{~size_t{0}};
        u64 map_serial{};
        const u8* host_ptr{};
    };
    std::array<PageCacheEntry, PageCacheSize> page_cache{};

    void ClearReadDescriptors() noexcept {
        page_cache = {};
        // Reserved capacity is not addressable. A later limit increase clears its new range.
        const size_t words = (std::min)(read_descriptors.size(), size_t{current_limit} / 64 + 1);
        std::fill_n(read_descriptors.begin(), words, u64{0});
        const size_t ids = (std::min)(bound_ids.size(), size_t{current_limit} + 1);
        std::fill_n(bound_ids.begin(), ids, Id{0});
    }
};

} // namespace VideoCommon
