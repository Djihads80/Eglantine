// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <tuple>

#include "common/common_funcs.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_page_heap.h"
#include "core/hle/kernel/k_typed_address.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace Kernel {

class KPageGroup;

class KMemoryManager {
public:
    enum class Pool : u32 {
        Application = 0,
        Applet = 1,
        System = 2,
        SystemNonSecure = 3,

        Count,

        Shift = 4,
        Mask = (0xF << Shift),

        // Aliases.
        Unsafe = Application,
        Secure = System,
    };

    enum class Direction : u32 {
        FromFront = 0,
        FromBack = 1,
        Shift = 0,
        Mask = (0xF << Shift),
    };

    static constexpr size_t MaxManagerCount = 10;

    explicit KMemoryManager(Core::System& system);

    void Initialize(KVirtualAddress management_region, size_t management_region_size);

    Result InitializeOptimizedMemory(u64 process_id, Pool pool);
    void FinalizeOptimizedMemory(u64 process_id, Pool pool);

    KPhysicalAddress AllocateAndOpenContinuous(size_t num_pages, size_t align_pages, u32 option);
    Result AllocateAndOpen(KPageGroup* out, size_t num_pages, u32 option);
    Result AllocateForProcess(KPageGroup* out, size_t num_pages, u32 option, u64 process_id,
                              u8 fill_pattern);

    /// Fills newly allocated pages. Zero fills skip pages that were never freed since boot: the
    /// host backing starts zeroed, and writing them would commit host memory for nothing.
    void FillAllocatedPages(KPhysicalAddress address, size_t num_pages, u8 fill_pattern);

    Pool GetPool(KPhysicalAddress address) const {
        return this->GetManager(address).GetPool();
    }

    void Open(KPhysicalAddress address, size_t num_pages) {
        // Repeatedly open references until we've done so for all pages.
        while (num_pages) {
            auto& manager = this->GetManager(address);
            const size_t cur_pages = (std::min)(num_pages, manager.GetPageOffsetToEnd(address));

            {
                KScopedLightLock lk(m_pool_locks[static_cast<size_t>(manager.GetPool())]);
                manager.Open(address, cur_pages);
            }

            num_pages -= cur_pages;
            address += cur_pages * PageSize;
        }
    }

    void OpenFirst(KPhysicalAddress address, size_t num_pages) {
        // Repeatedly open references until we've done so for all pages.
        while (num_pages) {
            auto& manager = this->GetManager(address);
            const size_t cur_pages = (std::min)(num_pages, manager.GetPageOffsetToEnd(address));

            {
                KScopedLightLock lk(m_pool_locks[static_cast<size_t>(manager.GetPool())]);
                manager.OpenFirst(address, cur_pages);
            }

            num_pages -= cur_pages;
            address += cur_pages * PageSize;
        }
    }

    void Close(KPhysicalAddress address, size_t num_pages) {
        // Pages that may be freed here may hold data: the next allocation clears them.
        this->MarkDirty(address, num_pages);

        // Repeatedly close references until we've done so for all pages.
        while (num_pages) {
            auto& manager = this->GetManager(address);
            const size_t cur_pages = (std::min)(num_pages, manager.GetPageOffsetToEnd(address));

            {
                KScopedLightLock lk(m_pool_locks[static_cast<size_t>(manager.GetPool())]);
                manager.Close(address, cur_pages);
            }

            num_pages -= cur_pages;
            address += cur_pages * PageSize;
        }
    }

    size_t GetSize() {
        size_t total = 0;
        for (size_t i = 0; i < m_num_managers; i++) {
            total += m_managers[i].GetSize();
        }
        return total;
    }

    size_t GetSize(Pool pool) {
        constexpr Direction GetSizeDirection = Direction::FromFront;
        size_t total = 0;
        for (auto* manager = this->GetFirstManager(pool, GetSizeDirection); manager != nullptr;
             manager = this->GetNextManager(manager, GetSizeDirection)) {
            total += manager->GetSize();
        }
        return total;
    }

    size_t GetFreeSize() {
        size_t total = 0;
        for (size_t i = 0; i < m_num_managers; i++) {
            KScopedLightLock lk(m_pool_locks[static_cast<size_t>(m_managers[i].GetPool())]);
            total += m_managers[i].GetFreeSize();
        }
        return total;
    }

    size_t GetFreeSize(Pool pool) {
        KScopedLightLock lk(m_pool_locks[static_cast<size_t>(pool)]);

        constexpr Direction GetSizeDirection = Direction::FromFront;
        size_t total = 0;
        for (auto* manager = this->GetFirstManager(pool, GetSizeDirection); manager != nullptr;
             manager = this->GetNextManager(manager, GetSizeDirection)) {
            total += manager->GetFreeSize();
        }
        return total;
    }

    void DumpFreeList(Pool pool) {
        KScopedLightLock lk(m_pool_locks[static_cast<size_t>(pool)]);

        constexpr Direction DumpDirection = Direction::FromFront;
        for (auto* manager = this->GetFirstManager(pool, DumpDirection); manager != nullptr;
             manager = this->GetNextManager(manager, DumpDirection)) {
            manager->DumpFreeList();
        }
    }

public:
    static size_t CalculateManagementOverheadSize(size_t region_size) {
        return Impl::CalculateManagementOverheadSize(region_size);
    }

    static constexpr u32 EncodeOption(Pool pool, Direction dir) {
        return (static_cast<u32>(pool) << static_cast<u32>(Pool::Shift)) |
               (static_cast<u32>(dir) << static_cast<u32>(Direction::Shift));
    }

    static constexpr Pool GetPool(u32 option) {
        return static_cast<Pool>((option & static_cast<u32>(Pool::Mask)) >>
                                 static_cast<u32>(Pool::Shift));
    }

    static constexpr Direction GetDirection(u32 option) {
        return static_cast<Direction>((option & static_cast<u32>(Direction::Mask)) >>
                                      static_cast<u32>(Direction::Shift));
    }

    static constexpr std::tuple<Pool, Direction> DecodeOption(u32 option) {
        return std::make_tuple(GetPool(option), GetDirection(option));
    }

private:
    class Impl {
    public:
        static size_t CalculateManagementOverheadSize(size_t region_size);

        static constexpr size_t CalculateOptimizedProcessOverheadSize(size_t region_size) {
            return (Common::AlignUp((region_size / PageSize), Common::BitSize<u64>()) /
                    Common::BitSize<u64>()) *
                   sizeof(u64);
        }

    public:
        Impl() = default;

        size_t Initialize(KPhysicalAddress address, size_t size, KVirtualAddress management,
                          KVirtualAddress management_end, Pool p);

        KPhysicalAddress AllocateBlock(s32 index, bool random) {
            return m_heap.AllocateBlock(index, random);
        }
        KPhysicalAddress AllocateAligned(s32 index, size_t num_pages, size_t align_pages) {
            return m_heap.AllocateAligned(index, num_pages, align_pages);
        }
        void Free(KPhysicalAddress addr, size_t num_pages) {
            m_heap.Free(addr, num_pages);
        }

        void SetInitialUsedHeapSize(size_t reserved_size) {
            m_heap.SetInitialUsedSize(reserved_size);
        }

        void InitializeOptimizedMemory(KernelCore& kernel);

        void TrackUnoptimizedAllocation(KernelCore& kernel, KPhysicalAddress block,
                                        size_t num_pages);
        void TrackOptimizedAllocation(KernelCore& kernel, KPhysicalAddress block, size_t num_pages);

        bool ProcessOptimizedAllocation(KernelCore& kernel, KPhysicalAddress block,
                                        size_t num_pages, u8 fill_pattern);

        constexpr Pool GetPool() const {
            return m_pool;
        }
        constexpr size_t GetSize() const {
            return m_heap.GetSize();
        }
        constexpr KPhysicalAddress GetEndAddress() const {
            return m_heap.GetEndAddress();
        }

        size_t GetFreeSize() const {
            return m_heap.GetFreeSize();
        }

        void DumpFreeList() const {
            UNIMPLEMENTED();
        }

        constexpr size_t GetPageOffset(KPhysicalAddress address) const {
            return m_heap.GetPageOffset(address);
        }
        constexpr size_t GetPageOffsetToEnd(KPhysicalAddress address) const {
            return m_heap.GetPageOffsetToEnd(address);
        }

        constexpr void SetNext(Impl* n) {
            m_next = n;
        }
        constexpr void SetPrev(Impl* n) {
            m_prev = n;
        }
        constexpr Impl* GetNext() const {
            return m_next;
        }
        constexpr Impl* GetPrev() const {
            return m_prev;
        }

        void OpenFirst(KPhysicalAddress address, size_t num_pages) {
            size_t index = this->GetPageOffset(address);
            const size_t end = index + num_pages;
            while (index < end) {
                const RefCount ref_count = (++m_page_reference_counts[index]);
                ASSERT(ref_count == 1);

                index++;
            }
        }

        void Open(KPhysicalAddress address, size_t num_pages) {
            size_t index = this->GetPageOffset(address);
            const size_t end = index + num_pages;
            while (index < end) {
                const RefCount ref_count = (++m_page_reference_counts[index]);
                ASSERT(ref_count > 1);

                index++;
            }
        }

        void Close(KPhysicalAddress address, size_t num_pages) {
            size_t index = this->GetPageOffset(address);
            const size_t end = index + num_pages;

            size_t free_start = 0;
            size_t free_count = 0;
            while (index < end) {
                ASSERT(m_page_reference_counts[index] > 0);
                const RefCount ref_count = (--m_page_reference_counts[index]);

                // Keep track of how many zero refcounts we see in a row, to minimize calls to free.
                if (ref_count == 0) {
                    if (free_count > 0) {
                        free_count++;
                    } else {
                        free_start = index;
                        free_count = 1;
                    }
                } else {
                    if (free_count > 0) {
                        this->MarkFreed(free_start, free_count);
                        this->Free(m_heap.GetAddress() + free_start * PageSize, free_count);
                        free_count = 0;
                    }
                }

                index++;
            }

            if (free_count > 0) {
                this->MarkFreed(free_start, free_count);
                this->Free(m_heap.GetAddress() + free_start * PageSize, free_count);
            }
        }

        void MarkAllFreed() {
            this->MarkFreed(0, this->GetSize() / PageSize);
        }

        /// Takes the freed mark off a page; true when it had one.
        bool TakeFreed(size_t index) {
            const u64 mask = u64{1} << (index % 64);
            return (m_freed_pages[index / 64].fetch_and(~mask, std::memory_order_relaxed) & mask) != 0;
        }

        /// Calls fn(first page, count) for each run of freed pages in [index, +num_pages) and takes
        /// their freed marks off.
        template <typename F>
        void TakeFreedRuns(size_t index, size_t num_pages, F&& fn) {
            size_t run_start = 0;
            size_t run_count = 0;
            ForEachWord(index, num_pages, [&](size_t word, u64 mask) {
                u64 freed = m_freed_pages[word].fetch_and(~mask, std::memory_order_relaxed) & mask;
                while (freed != 0) {
                    const size_t page = word * 64 + std::countr_zero(freed);
                    freed &= freed - 1;
                    if (run_count > 0 && run_start + run_count == page) {
                        ++run_count;
                        continue;
                    }
                    if (run_count > 0) {
                        fn(run_start, run_count);
                    }
                    run_start = page;
                    run_count = 1;
                }
            });
            if (run_count > 0) {
                fn(run_start, run_count);
            }
        }

    private:
        using RefCount = u16;

        void MarkFreed(size_t index, size_t num_pages) {
            ForEachWord(index, num_pages, [&](size_t word, u64 mask) {
                m_freed_pages[word].fetch_or(mask, std::memory_order_relaxed);
            });
        }

        /// Calls fn(word, mask) for the bits of [index, +num_pages) in each 64-page word.
        template <typename F>
        static void ForEachWord(size_t index, size_t num_pages, F&& fn) {
            const size_t end = index + num_pages;
            while (index < end) {
                const size_t bit = index % 64;
                const size_t count = (std::min)(64 - bit, end - index);
                const u64 mask = (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
                fn(index / 64, mask);
                index += count;
            }
        }

        KPageHeap m_heap;
        std::vector<RefCount> m_page_reference_counts;
        /// Pages that were in use and freed again, so they may hold data. Pages never handed out
        /// are still zero in the host backing. Atomic: fills run outside the pool lock.
        std::unique_ptr<std::atomic<u64>[]> m_freed_pages;
        KVirtualAddress m_management_region{};
        Pool m_pool{};
        Impl* m_next{};
        Impl* m_prev{};
    };

private:
    void MarkUsed(KPhysicalAddress address, size_t num_pages);
    void MarkDirty(KPhysicalAddress address, size_t num_pages);

    Impl& GetManager(KPhysicalAddress address) {
        return m_managers[m_memory_layout.GetPhysicalLinearRegion(address).GetAttributes()];
    }

    const Impl& GetManager(KPhysicalAddress address) const {
        return m_managers[m_memory_layout.GetPhysicalLinearRegion(address).GetAttributes()];
    }

    constexpr Impl* GetFirstManager(Pool pool, Direction dir) {
        return dir == Direction::FromBack ? m_pool_managers_tail[static_cast<size_t>(pool)]
                                          : m_pool_managers_head[static_cast<size_t>(pool)];
    }

    constexpr Impl* GetNextManager(Impl* cur, Direction dir) {
        if (dir == Direction::FromBack) {
            return cur->GetPrev();
        } else {
            return cur->GetNext();
        }
    }

    Result AllocatePageGroupImpl(KPageGroup* out, size_t num_pages, Pool pool, Direction dir,
                                 bool unoptimized, bool random);

private:
    template <typename T>
    using PoolArray = std::array<T, static_cast<size_t>(Pool::Count)>;

    Core::System& m_system;
    const KMemoryLayout& m_memory_layout;
    PoolArray<KLightLock> m_pool_locks;
    std::array<Impl*, MaxManagerCount> m_pool_managers_head{};
    std::array<Impl*, MaxManagerCount> m_pool_managers_tail{};
    std::array<Impl, MaxManagerCount> m_managers;
    size_t m_num_managers{};
    PoolArray<u64> m_optimized_process_ids{};
    PoolArray<bool> m_has_optimized_process{};
};

} // namespace Kernel
