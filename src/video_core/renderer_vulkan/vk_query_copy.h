// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <iterator>
#include <map>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vulkan {

// A query batch may write the same guest address repeatedly. vkCmdCopyBuffer forbids
// overlapping destinations in one command; retain the bytes from the last query write.
inline std::vector<VkBufferCopy> NonOverlappingQueryCopies(std::span<const VkBufferCopy> copies) {
    std::map<VkDeviceSize, VkBufferCopy> writes;
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        const VkDeviceSize end = copy.dstOffset + copy.size;
        auto it = writes.lower_bound(copy.dstOffset);
        if (it != writes.begin()) {
            const auto previous = std::prev(it);
            if (previous->second.dstOffset + previous->second.size > copy.dstOffset) {
                it = previous;
            }
        }
        while (it != writes.end() && it->first < end) {
            const VkBufferCopy old = it->second;
            it = writes.erase(it);
            const VkDeviceSize old_end = old.dstOffset + old.size;
            if (old.dstOffset < copy.dstOffset) {
                writes.emplace(old.dstOffset, VkBufferCopy{old.srcOffset, old.dstOffset,
                                                           copy.dstOffset - old.dstOffset});
            }
            if (old_end > end) {
                writes.emplace(
                    end, VkBufferCopy{old.srcOffset + end - old.dstOffset, end, old_end - end});
                break;
            }
        }
        writes.emplace(copy.dstOffset, copy);
    }
    std::vector<VkBufferCopy> result;
    result.reserve(writes.size());
    for (const auto& [offset, copy] : writes) {
        result.push_back(copy);
    }
    return result;
}

} // namespace Vulkan
