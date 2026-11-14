// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCommon {

struct ImageAllocationKey {
    PixelFormat format;
    Extent3D size;
    bool operator==(const ImageAllocationKey&) const noexcept = default;
};

inline std::optional<ImageAllocationKey> ReusableImageAllocationKey(const ImageInfo& info) {
    if (!info.native_tiling || info.type != ImageType::e2D || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.num_samples != 1 || info.is_sparse ||
        info.size.width == 0 || info.size.height == 0 || info.size.depth != 1 ||
        info.format == PixelFormat::Invalid || VideoCore::Surface::DefaultBlockWidth(info.format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(info.format) != 1) {
        return std::nullopt;
    }
    return ImageAllocationKey{info.format, info.size};
}

// Holds completed allocations only. Rejected resources remain owned by the caller.
template <typename Resource>
class ImageAllocationPool {
public:
    ImageAllocationPool(u64 max_bytes_, size_t max_entries_, u64 max_age_)
        : max_bytes{max_bytes_}, max_entries{max_entries_}, max_age{max_age_} {
        entries.reserve(max_entries);
    }

    template <typename IsFree>
    bool Recycle(const ImageAllocationKey& key, Resource& resource, u64 size, u64 tick,
                 IsFree&& is_free, u64 block_id = 0, u64 block_bytes = 0) {
        const u64 budget_bytes = block_bytes != 0 ? block_bytes : size;
        if (under_pressure || size == 0 || budget_bytes > max_bytes || max_entries == 0 ||
            !is_free(tick)) {
            return false;
        }
        const auto extra_bytes = [&] {
            return HasBlock(block_id) ? u64{0} : budget_bytes;
        };
        while (!entries.empty() &&
               (bytes > max_bytes - extra_bytes() || entries.size() >= max_entries)) {
            Remove(0);
            ++evicted;
        }
        bytes += extra_bytes();
        allocation_bytes += size;
        entries.push_back({key, std::move(resource), size, frame, block_id, budget_bytes});
        ++recycled;
        return true;
    }

    Resource Acquire(const ImageAllocationKey& key) {
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [&](const auto& entry) { return entry.key == key; });
        if (it == entries.end()) {
            ++misses;
            return {};
        }
        Resource resource = std::move(it->resource);
        Remove(static_cast<size_t>(it - entries.begin()));
        ++hits;
        return resource;
    }

    void Tick(bool pressure) {
        ++frame;
        under_pressure = pressure;
        if (pressure) {
            entries.clear();
            bytes = allocation_bytes = 0;
        } else {
            while (!entries.empty() && frame - entries.front().frame > max_age) {
                Remove(0);
            }
        }
    }

    u64 Bytes() const { return bytes; }
    u64 AllocationBytes() const { return allocation_bytes; }
    size_t Size() const { return entries.size(); }
    u64 Hits() const { return hits; }
    u64 Misses() const { return misses; }
    u64 Recycled() const { return recycled; }
    u64 Evicted() const { return evicted; }

private:
    struct Entry {
        ImageAllocationKey key;
        Resource resource;
        u64 bytes;
        u64 frame;
        u64 block_id;
        u64 budget_bytes;
    };
    bool HasBlock(u64 block_id, size_t ignore = (std::numeric_limits<size_t>::max)()) const {
        if (block_id == 0) {
            return false;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i != ignore && entries[i].block_id == block_id) {
                return true;
            }
        }
        return false;
    }
    void Remove(size_t index) {
        const auto& entry = entries[index];
        if (!HasBlock(entry.block_id, index)) {
            bytes -= entry.budget_bytes;
        }
        allocation_bytes -= entry.bytes;
        entries.erase(entries.begin() + index);
    }
    std::vector<Entry> entries;
    u64 max_bytes;
    size_t max_entries;
    u64 max_age;
    u64 bytes{};
    u64 allocation_bytes{};
    u64 frame{};
    u64 hits{};
    u64 misses{};
    u64 recycled{};
    u64 evicted{};
    bool under_pressure{};
};

} // namespace VideoCommon
