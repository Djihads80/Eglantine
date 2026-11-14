// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include "video_core/page_filter.h"
#include "common/container/unordered_map.h"
#include <utility>

#include "common/assert.h"
#include "common/bit_field.h"
#include "common/common_types.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/query_cache/query_base.h"
#include "video_core/query_cache/types.h"

namespace VideoCore {
class RasterizerInterface;
}

namespace Tegra {
class GPU;
}

namespace VideoCommon {

struct LookupData {
    VAddr address;
    QueryBase* found_query;
};

template <typename Traits>
class QueryCacheBase : public VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
    using RuntimeType = typename Traits::RuntimeType;

public:
    /// Device pages this cache ever registered objects on (InnerInvalidation skips the rest).
    VideoCommon::PageFilter page_filter;

    union QueryLocation {
        BitField<27, 5, u32> stream_id;
        BitField<0, 27, u32> query_id;
        u32 raw;

        std::pair<size_t, size_t> unpack() const {
            return {static_cast<size_t>(stream_id.Value()), static_cast<size_t>(query_id.Value())};
        }
    };

    explicit QueryCacheBase(Tegra::GPU& gpu, VideoCore::RasterizerInterface& rasterizer_,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_,
                            RuntimeType& runtime_);

    ~QueryCacheBase();

    void InvalidateRegion(VAddr addr, std::size_t size) {
        // Compute inline writes come by the thousand a frame, mostly where no report ever was:
        // pages never registered (page_filter) need no look under cache_mutex.
        if (!page_filter.Any(addr, size)) {
            return;
        }
        IterateCache<true>(addr, size,
                           [this](QueryLocation location) { InvalidateQuery(location); });
    }

    void FlushRegion(VAddr addr, std::size_t size) {
        if (SemiFlushRegion(addr, size)) {
            RequestGuestHostSync();
        }
    }

    /// Writes out the finalized reports in the region. True when some still wait for host results.
    bool SemiFlushRegion(VAddr addr, std::size_t size) {
        if (!page_filter.Any(addr, size)) {
            return false;
        }
        bool result = false;
        IterateCache<false>(addr, size, [this, &result](QueryLocation location) {
            result |= SemiFlushQueryDirty(location);
            return result;
        });
        return result;
    }

    static u64 BuildMask(std::span<const QueryType> types) {
        u64 mask = 0;
        for (auto query_type : types) {
            mask |= 1ULL << (static_cast<u64>(query_type));
        }
        return mask;
    }

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size) {
        if (!page_filter.Any(addr, size)) {
            return false;
        }
        bool result = false;
        IterateCache<false>(addr, size, [this, &result](QueryLocation location) {
            result |= IsQueryDirty(location);
            return result;
        });
        return result;
    }

    void CounterEnable(QueryType counter_type, bool is_enabled);

    void CounterReset(QueryType counter_type);

    void CounterClose(QueryType counter_type);

    void CounterReport(GPUVAddr addr, QueryType counter_type, QueryPropertiesFlags flags,
                       u32 payload, u32 subreport);

    void NotifyWFI();

    bool AccelerateHostConditionalRendering();

    // Async downloads
    void CommitAsyncFlushes();

    bool HasUncommittedFlushes() const;

    /// Whether a new fence has queries to flush or host writes to sync for this cache.
    bool HasPendingFenceWork() const;

    bool ShouldWaitAsyncFlushes();

    void PopAsyncFlushes();

    void NotifySegment(bool resume);

    void BindToChannel(s32 id) override;

protected:
    template <bool remove_from_cache, typename Func>
    void IterateCache(VAddr addr, std::size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result<Func, QueryLocation>, bool>;
        if (size == 0) {
            return;
        }
        const u64 addr_begin = addr;
        const u64 addr_end = addr_begin + size;

        // A read of a few words (the macro DMA reads of reports, ~1,700 a frame at Ocarina of
        // Time's water) looks up the offsets a query overlapping it can start at, instead of
        // walking every query of the page (hundreds there). Same queries, from the same pages.
        static constexpr u64 MaxProbes = 32;
        const u64 page_first = (addr_begin >> Core::DEVICE_PAGEBITS) << Core::DEVICE_PAGEBITS;
        const u64 probe_first =
            (std::max)(addr_begin >= sizeof(u32) - 1 ? addr_begin - (sizeof(u32) - 1) : 0,
                       page_first);
        if (addr_end > probe_first && addr_end - probe_first <= MaxProbes) {
            std::scoped_lock lock(cache_mutex);
            u64 cached_page = ~u64{0};
            ::Common::unordered_map<u32, QueryLocation>* contents = nullptr;
            for (u64 query_addr = probe_first; query_addr < addr_end; ++query_addr) {
                const u64 page = query_addr >> Core::DEVICE_PAGEBITS;
                if (page != cached_page) {
                    cached_page = page;
                    const auto it = cached_queries.find(page);
                    contents = it != cached_queries.end() ? &it->second : nullptr;
                }
                if (contents == nullptr) {
                    continue;
                }
                const auto query =
                    contents->find(static_cast<u32>(query_addr & Core::DEVICE_PAGEMASK));
                if (query == contents->end()) {
                    continue;
                }
                if constexpr (RETURNS_BOOL) {
                    if (func(query->second)) {
                        return;
                    }
                } else {
                    func(query->second);
                }
                if constexpr (remove_from_cache) {
                    contents->erase(query);
                    if (contents->empty()) {
                        cached_queries.erase(page);
                        contents = nullptr;
                    }
                }
            }
            return;
        }

        const u64 page_end = (addr_end - 1) >> Core::DEVICE_PAGEBITS;
        std::scoped_lock lock(cache_mutex);
        if (cached_queries.empty()) {
            return;
        }
        for (u64 page = addr_begin >> Core::DEVICE_PAGEBITS; page <= page_end; ++page) {
            const u64 page_start = page << Core::DEVICE_PAGEBITS;
            const auto in_range = [page_start, addr_begin, addr_end](const u32 query_location) {
                const u64 cache_begin = page_start + query_location;
                const u64 cache_end = cache_begin + sizeof(u32);
                return cache_begin < addr_end && addr_begin < cache_end;
            };
            const auto& it = cached_queries.find(page);
            if (it == std::end(cached_queries)) {
                continue;
            }
            auto& contents = it->second;
            bool matched = false;
            for (auto& query : contents) {
                if (!in_range(query.first)) {
                    continue;
                }
                matched = true;
                if constexpr (RETURNS_BOOL) {
                    if (func(query.second)) {
                        return;
                    }
                } else {
                    func(query.second);
                }
            }
            // Only pages with a query in the range: the scan above already found none elsewhere.
            if constexpr (remove_from_cache) {
                if (!matched) {
                    continue;
                }
                const auto in_range2 = [&](const std::pair<u32, QueryLocation>& pair) {
                    return in_range(pair.first);
                };
                erase_if(contents, in_range2);
                if (contents.empty()) {
                    cached_queries.erase(it);
                }
            }
        }
    }

    using ContentCache = ::Common::unordered_map<u64, ::Common::unordered_map<u32, QueryLocation>>;

    void InvalidateQuery(QueryLocation location);
    bool IsQueryDirty(QueryLocation location);
    bool SemiFlushQueryDirty(QueryLocation location);
    void RequestGuestHostSync();
    void UnregisterPending();

    ::Common::unordered_map<u64, ::Common::unordered_map<u32, QueryLocation>> cached_queries;
    std::mutex cache_mutex;

    struct QueryCacheBaseImpl;
    friend struct QueryCacheBaseImpl;
    friend RuntimeType;

    std::unique_ptr<QueryCacheBaseImpl> impl;
};

} // namespace VideoCommon
