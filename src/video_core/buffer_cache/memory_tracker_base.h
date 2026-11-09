// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <type_traits>
#include "common/container/unordered_set.h"
#include <utility>

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/buffer_cache/hot_pages.h"
#include "video_core/buffer_cache/word_manager.h"

namespace VideoCommon {

template <typename DeviceTracker>
class MemoryTrackerBase {
    static constexpr size_t MAX_CPU_PAGE_BITS = 36; // device addresses (MaxwellDeviceTraits)
    static constexpr size_t HIGHER_PAGE_BITS = 22;
    static constexpr size_t HIGHER_PAGE_SIZE = 1ULL << HIGHER_PAGE_BITS;
    static constexpr size_t HIGHER_PAGE_MASK = HIGHER_PAGE_SIZE - 1ULL;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;
    static constexpr size_t WORDS_STACK_NEEDED = HIGHER_PAGE_SIZE / BYTES_PER_WORD;
    using Manager = WordManager<DeviceTracker, WORDS_STACK_NEEDED, HIGHER_PAGE_SIZE>;

public:
    MemoryTrackerBase(DeviceTracker& device_tracker_) : device_tracker{&device_tracker_} {}
    ~MemoryTrackerBase() = default;

    /// Returns the inclusive CPU modified range in a begin end pair
    [[nodiscard]] std::pair<u64, u64> ModifiedCpuRegion(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePairs<true>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->ModifiedRegion(Type::CPU, offset, size);
        });
    }

    /// Returns the inclusive GPU modified range in a begin end pair
    [[nodiscard]] std::pair<u64, u64> ModifiedGpuRegion(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePairs<false>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->ModifiedRegion(Type::GPU, offset, size);
        });
    }

    /// Returns true if a region has been modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->IsRegionModified(Type::CPU, offset, size);
        });
    }

    /// Returns true if a region has been modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->IsRegionModified(Type::GPU, offset, size);
        });
    }

    /// Returns true if a region has pages CPU writes no longer fault on (written by the CPU and
    /// not uploaded since, or written through the cached path)
    [[nodiscard]] bool IsRegionUntracked(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->IsRegionModified(Type::Untracked, offset, size);
        });
    }

    /// Returns true if a region has been marked as Preflushable
    [[nodiscard]] bool IsRegionPreflushable(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(query_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            return manager->IsRegionModified(Type::Preflushable, offset, size);
        });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change. `write`: a
    /// CPU write did it (EDEN_NX2_HOT_PAGES history), not a buffer going away or a forced upload.
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size, bool write = false) {
        IteratePages<true>(dirty_cpu_addr, query_size, [this, write](Manager* manager, u64 offset, size_t size) {
            manager->ChangeRegionState(Type::CPU, true, manager->cpu_addr + offset, size, write);
            if (manager->hot_activity) {
                NoteHotActivity(manager);
            }
        });
    }

    /// Uploads the range again on its next synchronization, keeping its write protection (a
    /// forced re-upload, EDEN_NX2_HOT_PAGES bank re-uploads).
    void MarkRegionForUpload(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<true>(dirty_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            manager->MarkForUpload(manager->cpu_addr + offset, size);
        });
    }

    /// The CPU write generation of the region holding the range (see WordManager), or 0 when
    /// the range spans regions or has none yet. A range synchronized at one generation has
    /// nothing new to upload while its region's generation stays the same.
    [[nodiscard]] u64 RangeGeneration(VAddr query_cpu_addr, u64 query_size) const noexcept {
        const u64 page = query_cpu_addr >> HIGHER_PAGE_BITS;
        if (query_size == 0 || page != (query_cpu_addr + query_size - 1) >> HIGHER_PAGE_BITS ||
            page >= NUM_HIGH_PAGES || top_tier[page] == nullptr) {
            return 0;
        }
        const Manager* const manager = top_tier[page];
        // Guest writes to hot pages don't move the generation: such a range is never in sync.
        if (manager->hot_count != 0 &&
            manager->IsRegionModified(Type::Hot, query_cpu_addr & HIGHER_PAGE_MASK, query_size)) {
            return 0;
        }
        return manager->CpuGeneration();
    }

    /// Unmark region as CPU modified, notifying the device_tracker about this change
    void UnmarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<true>(dirty_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            manager->ChangeRegionState(Type::CPU, false, manager->cpu_addr + offset, size);
        });
    }

    /// Mark region as modified from the host GPU
    void MarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<true>(dirty_cpu_addr, query_size, [this](Manager* manager, u64 offset, size_t size) {
            if (manager->hot_tracking) {
                // Pages the GPU writes go back to write tracking and never turn hot again: a
                // CPU write to them has to be seen (downloads, GPU-modified ranges).
                manager->IterateWords(offset, size, [&](size_t index, u64 mask) {
                    const u64 hot = manager->Span(Type::Hot)[index] & mask;
                    if (hot != 0) {
                        DemoteHot(manager, index, hot);
                    }
                    manager->Span(Type::NoHot)[index] |= mask;
                });
            }
            manager->ChangeRegionState(Type::GPU, true, manager->cpu_addr + offset, size);
        });
    }

    /// Mark region as modified from the host GPU
    void MarkRegionAsPreflushable(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<true>(dirty_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            manager->ChangeRegionState(Type::Preflushable, true, manager->cpu_addr + offset, size);
        });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<true>(dirty_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            manager->ChangeRegionState(Type::GPU, false, manager->cpu_addr + offset, size);
        });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsPreflushable(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<true>(dirty_cpu_addr, query_size, [](Manager* manager, u64 offset, size_t size) {
            manager->ChangeRegionState(Type::Preflushable, false, manager->cpu_addr + offset, size);
        });
    }

    /// Mark region as modified from the CPU
    /// but don't mark it as modified until FlusHCachedWrites is called.
    void CachedCpuWrite(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<true>(dirty_cpu_addr, query_size, [this](Manager* manager, u64 offset, size_t size) {
            const VAddr cpu_address = manager->cpu_addr + offset;
            manager->ChangeRegionState(Type::CachedCPU, true, cpu_address, size);
            cached_pages.insert(u32(cpu_address >> HIGHER_PAGE_BITS));
        });
    }

    /// Flushes cached CPU writes, and notify the device_tracker about the deltas
    void FlushCachedWrites(VAddr query_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(query_cpu_addr, query_size, [](Manager* manager, [[maybe_unused]] u64 offset, [[maybe_unused]] size_t size) {
            manager->FlushCachedWrites();
        });
    }

    void FlushCachedWrites() noexcept {
        for (auto id : cached_pages) {
            top_tier[id]->FlushCachedWrites();
        }
        cached_pages.clear();
    }

    /// False when every 4 MiB page in the range exists and has no CPU or untracked bits.
    /// A missing page still needs the upload walk: creating it treats the page as dirty.
    [[nodiscard]] bool HasPendingCpuUpload(VAddr query_cpu_addr, u64 query_size) const noexcept {
        if (query_size == 0) {
            return false;
        }
        std::size_t remaining_size{query_size};
        std::size_t page_index{query_cpu_addr >> HIGHER_PAGE_BITS};
        u64 page_offset{query_cpu_addr & HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            const Manager* const manager{top_tier[page_index]};
            if (manager == nullptr || manager->HasPendingUpload()) {
                return true;
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    /// EDEN_NX2_HOT_PAGES: hot pages exist (ForEachHotUpload has work).
    [[nodiscard]] bool HasHotPages() const noexcept {
        return hot_total != 0;
    }

    /// EDEN_NX2_HOT_PAGES: synchronizes the hot pages of a range, which ForEachUploadRange
    /// skips. Each one is compared with its shadow (the bytes its GPU copy came from); when
    /// they differ, or it has no shadow yet, the shadow is refreshed from guest memory and
    /// func(page_address, shadow_bytes) uploads the page from it. Pages that are not whole in
    /// [buffer_begin, buffer_end) or not mapped go back to write tracking (normal upload).
    /// Call it before ForEachUploadRange on the same range.
    template <typename Func>
    void ForEachHotUpload(VAddr query_cpu_range, u64 query_size, VAddr buffer_begin,
                          VAddr buffer_end, Func&& func) {
        if (hot_total == 0) {
            return;
        }
        IteratePages<false>(query_cpu_range, query_size, [&](Manager* manager, u64 offset, size_t size) {
            if (manager->hot_count == 0) {
                return;
            }
            manager->IterateWords(offset, size, [&](size_t index, u64 mask) {
                u64 hot = manager->Span(Type::Hot)[index] & mask;
                while (hot != 0) {
                    const u64 bit = hot & (~hot + 1);
                    hot &= hot - 1;
                    const VAddr page = manager->cpu_addr +
                                       (index * PAGES_PER_WORD + std::countr_zero(bit)) * BYTES_PER_PAGE;
                    const u8* const guest = GuestPage(page);
                    u8* const shadow = shadows.Find(page);
                    if (page < buffer_begin || page + BYTES_PER_PAGE > buffer_end ||
                        guest == nullptr || shadow == nullptr) {
                        // No upload this buffer took can be vouched for: CPU-dirty.
                        manager->Span(Type::HotValid)[index] &= ~bit;
                        DemoteHot(manager, index, bit);
                        manager->Span(Type::NoHot)[index] |= bit;
                        continue;
                    }
                    ++hot_stats.compares;
                    u64& valid = manager->Span(Type::HotValid)[index];
                    if ((valid & bit) != 0 && std::memcmp(guest, shadow, BYTES_PER_PAGE) == 0) {
                        ++hot_stats.unchanged;
                        continue;
                    }
                    std::memcpy(shadow, guest, BYTES_PER_PAGE);
                    valid |= bit;
                    manager->Span(Type::Faulted)[index] |= bit;
                    NoteHotActivity(manager);
                    ++hot_stats.uploads;
                    func(page, static_cast<const u8*>(shadow));
                }
            });
        });
    }

    /// EDEN_NX2_HOT_PAGES: the GPU copy of the range now holds these bytes (inline uploads,
    /// written to guest memory too): hot pages' shadows take them, so the next comparison is
    /// against what the GPU has.
    void UpdateHotShadows(VAddr cpu_addr, std::span<const u8> bytes) {
        if (hot_total == 0 || bytes.empty()) {
            return;
        }
        IteratePages<false>(cpu_addr, bytes.size(), [&](Manager* manager, u64 offset, size_t size) {
            if (manager->hot_count == 0) {
                return;
            }
            manager->IterateWords(offset, size, [&](size_t index, u64 mask) {
                u64 pages = manager->Span(Type::Hot)[index] & manager->Span(Type::HotValid)[index] & mask;
                while (pages != 0) {
                    const u64 bit = pages & (~pages + 1);
                    pages &= pages - 1;
                    const VAddr page = manager->cpu_addr +
                                       (index * PAGES_PER_WORD + std::countr_zero(bit)) * BYTES_PER_PAGE;
                    u8* const shadow = shadows.Find(page);
                    if (shadow == nullptr) {
                        continue;
                    }
                    const VAddr begin = (std::max)(page, cpu_addr);
                    const VAddr end = (std::min)(page + BYTES_PER_PAGE, cpu_addr + bytes.size());
                    std::memcpy(shadow + (begin - page), bytes.data() + (begin - cpu_addr), end - begin);
                }
            });
        });
    }

    /// EDEN_NX2_HOT_PAGES: hot pages of the range must be uploaded again on their next
    /// synchronization (their buffer was just created).
    void InvalidateHotShadows(VAddr cpu_addr, u64 size) {
        if (hot_total == 0) {
            return;
        }
        IteratePages<false>(cpu_addr, size, [](Manager* manager, u64 offset, size_t range) {
            if (manager->hot_count == 0) {
                return;
            }
            manager->IterateWords(offset, range, [&](size_t index, u64 mask) {
                manager->Span(Type::HotValid)[index] &= ~mask;
            });
        });
    }

    /// EDEN_NX2_HOT_PAGES, once a frame: moves the fault history along, turns pages whose writes
    /// faulted in enough of the last four frames hot (eligible(page) vetoes) and returns hot
    /// pages unchanged for four frames to write tracking.
    template <typename Eligible>
    void TickHotPages(Eligible&& eligible) {
        if (!HotPages::Enabled()) {
            return;
        }
        const u32 min_frames = HotPages::MinFrames();
        const size_t max_pages = HotPages::MaxPages();
        size_t kept = 0;
        for (size_t i = 0; i < hot_list.size(); ++i) {
            Manager* const manager = hot_list[i];
            manager->hot_activity = false;
            bool any = false;
            for (size_t index = 0; index < Manager::num_words; ++index) {
                u64& faulted = manager->Span(Type::Faulted)[index];
                u64& h0 = manager->Span(Type::Hist0)[index];
                u64& h1 = manager->Span(Type::Hist1)[index];
                u64& h2 = manager->Span(Type::Hist2)[index];
                u64& h3 = manager->Span(Type::Hist3)[index];
                h3 = h2;
                h2 = h1;
                h1 = h0;
                h0 = std::exchange(faulted, 0);
                const u64 hot = manager->Span(Type::Hot)[index];
                const u64 quiet = hot & ~(h0 | h1 | h2 | h3);
                if (quiet != 0) {
                    DemoteHot(manager, index, quiet);
                }
                const u64 candidates = HotPages::AtLeast(min_frames, h0, h1, h2, h3) & ~hot;
                const u64 blocked = candidates & (manager->Span(Type::NoHot)[index] |
                                                  manager->Span(Type::GPU)[index]);
                hot_stats.blocked += static_cast<u64>(std::popcount(blocked));
                u64 promote = candidates & ~blocked;
                while (promote != 0) {
                    const u64 bit = promote & (~promote + 1);
                    promote &= promote - 1;
                    const VAddr page = manager->cpu_addr +
                                       (index * PAGES_PER_WORD + std::countr_zero(bit)) * BYTES_PER_PAGE;
                    if (!eligible(page)) {
                        manager->Span(Type::NoHot)[index] |= bit;
                        ++hot_stats.refused;
                        continue;
                    }
                    if (hot_total >= max_pages) {
                        ++hot_stats.capped;
                        continue;
                    }
                    [[maybe_unused]] u8* const shadow = shadows.Acquire(page);
                    manager->Span(Type::Hot)[index] |= bit;
                    ++manager->hot_count;
                    ++hot_total;
                    ++hot_stats.promoted;
                    // CPU-dirty and unprotected from now on (no shadow yet: uploaded at its next
                    // synchronization).
                    manager->ChangeRegionState(Type::CPU, true, page, BYTES_PER_PAGE);
                }
                any = any || (h0 | h1 | h2 | h3 | manager->Span(Type::Hot)[index] |
                              manager->Span(Type::Faulted)[index]) != 0;
            }
            if (any || manager->hot_activity) {
                hot_list[kept++] = manager;
            } else {
                manager->hot_listed = false;
            }
        }
        hot_list.resize(kept);
    }

    /// EDEN_NX2_HOT_PAGES statistics (buffer cache side).
    [[nodiscard]] const HotPages::Stats& HotStats() const noexcept {
        return hot_stats;
    }

    [[nodiscard]] size_t HotPageCount() const noexcept {
        return hot_total;
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    template <typename Func>
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, Func&& func) {
        IteratePages<true>(query_cpu_range, query_size, [&func](Manager* manager, u64 offset, size_t size) {
            manager->ForEachModifiedRange(Type::CPU, true, manager->cpu_addr + offset, size, func);
        });
    }

    /// Call 'func' for each range marked as Preflushable
    template <typename Func>
    void ForEachPreflushableRange(VAddr query_cpu_range, u64 query_size, Func&& func) {
        IteratePages<false>(query_cpu_range, query_size, [&func](Manager* manager, u64 offset, size_t size) {
            manager->ForEachModifiedRange(Type::Preflushable, false, manager->cpu_addr + offset, size, func);
        });
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <typename Func>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, bool clear, Func&& func) {
        IteratePages<false>(query_cpu_range, query_size, [&func, clear](Manager* manager, u64 offset, size_t size) {
            manager->ForEachModifiedRange(Type::GPU, clear, manager->cpu_addr + offset, size, func);
        });
    }

    template <typename Func>
    void ForEachDownloadRangeAndClear(VAddr query_cpu_range, u64 query_size, Func&& func) {
        IteratePages<false>(query_cpu_range, query_size, [&func](Manager* manager, u64 offset, size_t size) {
            manager->ForEachModifiedRange(Type::GPU, true, manager->cpu_addr + offset, size, func);
        });
    }

private:
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, Manager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    template <bool create_region_on_fail, typename Func>
    std::pair<u64, u64> IteratePairs(VAddr cpu_address, size_t size, Func&& func) {
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & HIGHER_PAGE_MASK};
        u64 begin = (std::numeric_limits<u64>::max)();
        u64 end = 0;
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            const auto execute = [&] {
                auto [new_begin, new_end] = func(manager, page_offset, copy_amount);
                if (new_begin != 0 || new_end != 0) {
                    const u64 base_address = page_index << HIGHER_PAGE_BITS;
                    begin = (std::min)(new_begin + base_address, begin);
                    end = (std::max)(new_end + base_address, end);
                }
            };
            if (manager) {
                execute();
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                execute();
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        if (begin < end) {
            return std::make_pair(begin, end);
        } else {
            return std::make_pair(0ULL, 0ULL);
        }
    }

    /// Guest memory of a device page (nullptr when unmapped, or for trackers without memory).
    [[nodiscard]] const u8* GuestPage(VAddr page) {
        if constexpr (requires(DeviceTracker& tracker, VAddr address) {
                          tracker.template GetPointer<u8>(address);
                      }) {
            return device_tracker->template GetPointer<u8>(page);
        } else {
            return nullptr;
        }
    }

    void NoteHotActivity(Manager* manager) {
        if (!manager->hot_listed) {
            manager->hot_listed = true;
            hot_list.push_back(manager);
        }
    }

    /// Returns hot pages (bits of word `index`) to write tracking. A page whose GPU copy matches
    /// its shadow is marked clean (write-protected again) and then compared with guest memory:
    /// a write before the protection shows as a difference and marks it CPU-dirty again, one
    /// after it faults as usual. Any other page stays CPU-dirty and is uploaded on its next
    /// synchronization.
    void DemoteHot(Manager* manager, size_t index, u64 bits) {
        u64& hot = manager->Span(Type::Hot)[index];
        bits &= hot;
        if (bits == 0) {
            return;
        }
        hot &= ~bits;
        const u64 valid = manager->Span(Type::HotValid)[index] & bits;
        manager->Span(Type::HotValid)[index] &= ~bits;
        const u32 count = static_cast<u32>(std::popcount(bits));
        manager->hot_count -= count;
        hot_total -= count;
        hot_stats.demoted += count;
        for (u64 pages = bits; pages != 0; pages &= pages - 1) {
            const VAddr page = manager->cpu_addr +
                               (index * PAGES_PER_WORD + std::countr_zero(pages)) * BYTES_PER_PAGE;
            const u8* const shadow = shadows.Find(page);
            if ((valid & (pages & (~pages + 1))) != 0 && shadow != nullptr) {
                manager->ChangeRegionState(Type::CPU, false, page, BYTES_PER_PAGE);
                const u8* const guest = GuestPage(page);
                if (guest == nullptr || std::memcmp(guest, shadow, BYTES_PER_PAGE) != 0) {
                    manager->ChangeRegionState(Type::CPU, true, page, BYTES_PER_PAGE, true);
                }
            }
            shadows.Release(page);
        }
        NoteHotActivity(manager);
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << HIGHER_PAGE_BITS;
        top_tier[page_index] = GetNewManager(base_cpu_addr);
    }

    Manager* GetNewManager(VAddr base_cpu_address) {
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                new (&last_pool[i]) Manager(0, *device_tracker);
                free_managers.push_back(&last_pool[i]);
            }
        }
        Manager* new_manager = free_managers.front();
        new_manager->cpu_addr = base_cpu_address;
        new_manager->hot_tracking = HotPages::Enabled();
        free_managers.pop_front();
        return new_manager;
    }

    std::array<Manager*, NUM_HIGH_PAGES> top_tier{};
    std::deque<std::array<Manager, MANAGER_POOL_SIZE>> manager_pool;
    std::deque<Manager*> free_managers;
    ::Common::unordered_set<u32> cached_pages;
    DeviceTracker* device_tracker = nullptr;
    // EDEN_NX2_HOT_PAGES
    std::vector<Manager*> hot_list; ///< managers with fault history or hot pages
    HotPages::ShadowStore shadows;
    HotPages::Stats hot_stats;
    size_t hot_total = 0;
};

} // namespace VideoCommon
