// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/common_types.h"
#include "common/heap_tracker.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2_war_probe.h"
#include "video_core/rasterizer_download_area.h"

namespace Core::Memory {

u32& UnmappedReadFlag() {
    thread_local u32 flag{};
    return flag;
}

static inline bool AddressSpaceContains(const Common::PageTable& table, const Common::ProcessAddress addr, const std::size_t size) {
    const Common::ProcessAddress max_addr = 1ULL << table.GetAddressSpaceBits();
    return addr + size >= addr && addr + size <= max_addr;
}

// Implementation class used to keep the specifics of the memory subsystem hidden
// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl {
    explicit Impl(Core::System& system_) : system{system_} {}

    void SetCurrentPageTable(Kernel::KProcess& process) {
        current_page_table = &process.GetPageTable().GetImpl();

        if (process.IsApplication() && Settings::IsFastmemEnabled()) {
            current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer();
        } else {
            current_page_table->fastmem_arena = nullptr;
        }

#ifdef __ANDROID__
        heap_tracker.emplace(system.DeviceMemory().buffer);
        host_buffer = std::addressof(*heap_tracker);
#else
        host_buffer = std::addressof(system.DeviceMemory().buffer);
#endif
        const bool war_capable =
            process.IsApplication() && current_page_table->fastmem_arena != nullptr &&
            Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce &&
            current_page_table->GetAddressSpaceBits() <= 43;
        war_inflight = war_capable && VideoCommon::Nx2Inflight::Any();
        war_probe = war_capable && (VideoCommon::Nx2WarProbe::On() || war_inflight);
        if (war_probe) {
            war_unprotected.Init();
        }
    }

    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}",
                   GetInteger(target));
        std::unique_lock war_lock{war_mutex, std::defer_lock};
        if (war_probe && &page_table == current_page_table) [[unlikely]] {
            war_lock.lock();
            WarForget(GetInteger(base), size);
        }
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, target,
                 Common::PageType::Memory);

        if (current_page_table->fastmem_arena) {
            host_buffer->Map(GetInteger(base), GetInteger(target) - DramMemoryMap::Base, size, perms, separate_heap);
        }
    }

    void UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                     bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        std::unique_lock war_lock{war_mutex, std::defer_lock};
        if (war_probe && &page_table == current_page_table) [[unlikely]] {
            war_lock.lock();
            WarForget(GetInteger(base), size);
        }
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0,
                 Common::PageType::Unmapped);

        if (current_page_table->fastmem_arena) {
            host_buffer->Unmap(GetInteger(base), size, separate_heap);
        }
    }

    void ProtectRegion(Common::PageTable& page_table, VAddr vaddr, u64 size,
                       Common::MemoryPermission perms) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((vaddr & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", vaddr);

        if (!current_page_table->fastmem_arena) {
            return;
        }
        std::unique_lock war_lock{war_mutex, std::defer_lock};
        if (war_probe && &page_table == current_page_table) [[unlikely]] {
            war_lock.lock();
            WarForget(vaddr, size);
        }

        u64 protect_bytes = 0, protect_begin = 0;

        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr + size) >> YUZU_PAGEBITS);
        for (u64 addr = vaddr; addr < vaddr + size; addr += YUZU_PAGESIZE) {
            const Common::PageType page_type = current_page_table->entries.GetUnchecked(addr >> YUZU_PAGEBITS).Type();
            switch (page_type) {
            case Common::PageType::RasterizerCachedMemory:
                if (protect_bytes > 0) {
                    host_buffer->Protect(protect_begin, protect_bytes, perms);
                    protect_bytes = 0;
                }
                break;
            default:
                if (protect_bytes == 0)
                    protect_begin = addr;
                protect_bytes += YUZU_PAGESIZE;
            }
        }

        if (protect_bytes > 0) {
            host_buffer->Protect(protect_begin, protect_bytes, perms);
        }
    }

    [[nodiscard]] u8* GetPointerFromRasterizerCachedMemory(u64 vaddr) const {
        if (u64 paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Pointer(true); paddr)
            return reinterpret_cast<u8*>(paddr) + vaddr;
        return {};
    }

    [[nodiscard]] u8* GetPointerFromDebugMemory(u64 vaddr) const {
        if (u64 paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Pointer(true); paddr)
            return reinterpret_cast<u8*>(paddr) + vaddr;
        return {};
    }

    u8 Read8(const Common::ProcessAddress addr) {
        return Read<u8>(addr);
    }

    u16 Read16(const Common::ProcessAddress addr) {
        if ((addr & 1) == 0) {
            return Read<u16_le>(addr);
        } else {
            const u32 a{Read<u8>(addr)};
            const u32 b{Read<u8>(addr + sizeof(u8))};
            return static_cast<u16>((b << 8) | a);
        }
    }

    u32 Read32(const Common::ProcessAddress addr) {
        if ((addr & 3) == 0) {
            return Read<u32_le>(addr);
        } else {
            const u32 a{Read16(addr)};
            const u32 b{Read16(addr + sizeof(u16))};
            return (b << 16) | a;
        }
    }

    u64 Read64(const Common::ProcessAddress addr) {
        if ((addr & 7) == 0) {
            return Read<u64_le>(addr);
        } else {
            const u32 a{Read32(addr)};
            const u32 b{Read32(addr + sizeof(u32))};
            return (static_cast<u64>(b) << 32) | a;
        }
    }

    void Write8(const Common::ProcessAddress addr, const u8 data) {
        Write<u8>(addr, data);
    }

    void Write16(const Common::ProcessAddress addr, const u16 data) {
        if ((addr & 1) == 0) {
            Write<u16_le>(addr, data);
        } else {
            Write<u8>(addr, static_cast<u8>(data));
            Write<u8>(addr + sizeof(u8), static_cast<u8>(data >> 8));
        }
    }

    void Write32(const Common::ProcessAddress addr, const u32 data) {
        if ((addr & 3) == 0) {
            Write<u32_le>(addr, data);
        } else {
            Write16(addr, static_cast<u16>(data));
            Write16(addr + sizeof(u16), static_cast<u16>(data >> 16));
        }
    }

    void Write64(const Common::ProcessAddress addr, const u64 data) {
        if ((addr & 7) == 0) {
            Write<u64_le>(addr, data);
        } else {
            Write32(addr, static_cast<u32>(data));
            Write32(addr + sizeof(u32), static_cast<u32>(data >> 32));
        }
    }

    bool WriteExclusive8(const Common::ProcessAddress addr, const u8 data, const u8 expected) {
        return WriteExclusive<u8>(addr, data, expected);
    }

    bool WriteExclusive16(const Common::ProcessAddress addr, const u16 data, const u16 expected) {
        return WriteExclusive<u16_le>(addr, data, expected);
    }

    bool WriteExclusive32(const Common::ProcessAddress addr, const u32 data, const u32 expected) {
        return WriteExclusive<u32_le>(addr, data, expected);
    }

    bool WriteExclusive64(const Common::ProcessAddress addr, const u64 data, const u64 expected) {
        return WriteExclusive<u64_le>(addr, data, expected);
    }

    std::string ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
        std::string string;
        string.reserve(max_length);
        for (std::size_t i = 0; i < max_length; ++i) {
            const char c = Read<s8>(vaddr);
            if (c == '\0') {
                break;
            }
            string.push_back(c);
            ++vaddr;
        }
        string.shrink_to_fit();
        return string;
    }

    template<typename F1, typename F2, typename F3>
    inline bool WalkBlock(const Common::ProcessAddress addr, const std::size_t size, F1&& on_unmapped, F2&& on_memory, F3&& on_rasterizer) {
        std::size_t offset = 0;
        if (!AddressSpaceContains(*current_page_table, addr, size)) {
            on_unmapped(offset, size, addr);
            return false;
        }
        std::size_t remaining_size = size;
        std::size_t page_index = addr >> YUZU_PAGEBITS;
        std::size_t page_offset = addr & YUZU_PAGEMASK;
        bool user_accessible = true;

        current_page_table->entries.CommitRegion(page_index, page_index + (size >> YUZU_PAGEBITS) + 1);
        while (remaining_size != 0) {
            const std::size_t copy_amount = (std::min)(std::size_t(YUZU_PAGESIZE) - page_offset, remaining_size);
            const auto current_vaddr = u64((page_index << YUZU_PAGEBITS) + page_offset);
            const auto [pointer, type, _] = current_page_table->entries.GetUnchecked(page_index).PointerTypeBlock();
            switch (type) {
            case Common::PageType::Unmapped: {
                user_accessible = false;
                on_unmapped(offset, copy_amount, current_vaddr);
                break;
            }
            case Common::PageType::Memory: {
                u8* mem_ptr = reinterpret_cast<u8*>(pointer + page_offset + (page_index << YUZU_PAGEBITS));
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::DebugMemory: {
                u8* const mem_ptr = GetPointerFromDebugMemory(current_vaddr);
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromRasterizerCachedMemory(current_vaddr);
                on_rasterizer(current_vaddr, offset, copy_amount, host_ptr);
                break;
            }
            default:
                UNREACHABLE();
            }
            page_index++;
            page_offset = 0;
            offset += copy_amount;
            remaining_size -= copy_amount;
        }
        return user_accessible;
    }

    bool ReadBlockImpl(const Common::ProcessAddress addr, void* buffer, const std::size_t size, bool unsafe) {
        return WalkBlock(addr, size,
        [addr, size, &buffer](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
            std::memset(reinterpret_cast<u8*>(buffer) + offset, 0, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        });
    }

    [[nodiscard]] inline const u8* GetSpan(const VAddr addr, const std::size_t size) const noexcept {
        const auto first = current_page_table->entries[addr >> YUZU_PAGEBITS].Block();
        const auto last = current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].Block();
        if (Common::PageTable::PageEntryData::CanFastSpan(first, last)) {
            return GetPointerSilent(addr);
        }
        if (first != Common::PageTable::PageEntryData::INVALID_BLOCK_ID &&
            last != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
            return nullptr;
        }
        return GetContiguousSpan(addr, size);
    }
    [[nodiscard]] inline u8* GetSpan(const VAddr addr, const std::size_t size) noexcept {
        const auto first = current_page_table->entries[addr >> YUZU_PAGEBITS].Block();
        const auto last = current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].Block();
        if (Common::PageTable::PageEntryData::CanFastSpan(first, last)) {
            return GetPointerSilent(addr);
        }
        if (first != Common::PageTable::PageEntryData::INVALID_BLOCK_ID &&
            last != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
            return nullptr;
        }
        return GetContiguousSpan(addr, size);
    }

    bool WriteBlockImpl(const Common::ProcessAddress addr, const void* buffer, const std::size_t size, bool unsafe) {
        if (war_inflight) [[unlikely]] {
            WarOnWrite(GetInteger(addr), size);
        }
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        });
    }

    bool ZeroBlock(const Common::ProcessAddress addr, const std::size_t size) {
        if (war_inflight) [[unlikely]] {
            WarOnWrite(GetInteger(addr), size);
        }
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [=](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memset(ptr, 0, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            std::memset(ptr, 0, copy_amount);
        });
    }

    bool CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr, const std::size_t size) {
        return WalkBlock(dest_addr, size,
        [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(src_addr), size);
            ZeroBlock(dest_addr + offset, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        });
    }

    template <typename Callback>
    Result PerformCacheOperation(Common::ProcessAddress dest_addr, std::size_t size, Callback&& cb) {
        bool had_unmapped = false;
        WalkBlock(dest_addr, size,
            [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory, "Unmapped @ {:#018X}", GetInteger(current_vaddr));
                had_unmapped = true;
            },
            [](const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) {},
            [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) { cb(current_vaddr, offset, copy_amount); }
        );
        return had_unmapped ? Kernel::ResultInvalidCurrentMemory : ResultSuccess;
    }

    Result InvalidateDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc ivac: Invalidate to point of coherency
            // GPU flush -> CPU invalidate
            HandleRasterizerDownload(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result StoreDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc cvac: Store to point of coherency
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result FlushDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc civac: Store to point of coherency, and invalidate from cache
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    void MarkRegionDebug(u64 vaddr, u64 size, bool debug) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }
        std::unique_lock war_lock{war_mutex, std::defer_lock};
        if (war_probe) [[unlikely]] {
            war_lock.lock();
            WarForget(vaddr, size);
        }

        if (current_page_table->fastmem_arena) {
            const auto perm{debug ? Common::MemoryPermission{}
                                  : Common::MemoryPermission::ReadWrite};
            host_buffer->Protect(vaddr, size, perm);
        }

        // Iterate over a contiguous CPU address space, marking/unmarking the region.
        // The region is at a granularity of CPU pages.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;

        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr >> YUZU_PAGEBITS) + num_pages);
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            auto& entry = current_page_table->entries.GetUnchecked(vaddr >> YUZU_PAGEBITS);
            const auto [pointer, type, block] = entry.PointerTypeBlock(true);
            if (debug) {
                // Switch page type to debug if now debug
                switch (type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::DebugMemory:
                    // Page is already marked.
                    break;
                case Common::PageType::Memory:
                    entry.MarkDebug(pointer, block);
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to non-debug if now non-debug
                switch (type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as non-debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::Memory:
                    // Don't mess with already non-debug or rasterizer memory.
                    break;
                case Common::PageType::DebugMemory: {
                    entry.Store(false, Common::PageType::Memory, block, pointer);
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    void RasterizerMarkRegionCached(u64 vaddr, u64 size, bool cached) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }
        // war_guard_probe: arming and trap restores never interleave with the tracker's changes.
        std::unique_lock war_lock{war_mutex, std::defer_lock};
        const u64 war_vaddr = vaddr;
        if (war_probe) [[unlikely]] {
            war_lock.lock();
        }
        SCOPE_EXIT {
            if (war_lock.owns_lock()) {
                WarTrackerMarked(war_vaddr, size, cached);
            }
        };

        if (current_page_table->fastmem_arena) {
            Common::MemoryPermission perm{};
            // EDEN_NX2_NO_REACTIVE=1 drops the read traps on Switch 2 programs. Opt-in: without
            // them a CPU read of GPU-written memory can see stale data.
            static const bool nx2_no_reactive = [] {
                const char* value = std::getenv("EDEN_NX2_NO_REACTIVE");
                return value != nullptr && value[0] == '1';
            }();
            const bool reactive_flushing =
                Settings::values.use_reactive_flushing.GetValue() &&
                !(nx2_no_reactive && Settings::CurrentProgramIsSwitch2());
            if (!reactive_flushing || !cached) {
                perm |= Common::MemoryPermission::Read;
            }
            if (!cached) {
                perm |= Common::MemoryPermission::Write;
            }
            host_buffer->Protect(vaddr, size, perm);
            if (cached && perm != Common::MemoryPermission{}) {
                ProtectArmedPages(vaddr, size);
            }
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr >> YUZU_PAGEBITS) + num_pages);
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            auto& entry = current_page_table->entries.GetUnchecked(vaddr >> YUZU_PAGEBITS);
            const Common::PageType page_type = entry.Type();
            if (cached) {
                // Switch page type to cached if now cached
                switch (page_type) {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    entry.MarkRasterizerCached();
                    break;
                case Common::PageType::RasterizerCachedMemory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already marked as cached.
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to uncached if now uncached
                switch (page_type) {
                case Common::PageType::Unmapped: // NOLINT(bugprone-branch-clone)
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already unmarked as cached.
                    break;
                case Common::PageType::RasterizerCachedMemory: {
                    if (auto [ptr, _, block] = entry.PointerTypeBlock(true); ptr == 0) {
                        // It's possible that this function has been called while updating the
                        // pagetable after unmapping a VMA. In that case the underlying VMA will no
                        // longer exist, and we should just leave the pagetable entry blank.
                        entry.Store(false, Common::PageType::Unmapped, block, 0);
                    } else {
                        entry.Store(false, Common::PageType::Memory, block, ptr);
                    }
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    /// Read traps replace reactive flushing's blanket read protection of every cached page: only
    /// pages the GPU writes are protected, and each one only until its first read
    /// (EDEN_NX2_READ_TRAPS=0 turns them off). Under NCE every fault is taken as a write, so they
    /// are not used there.
    bool ReadTrapsEnabled() const {
        static const bool disabled = [] {
            const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
            return value != nullptr && value[0] == '0';
        }();
        return !disabled && current_page_table->fastmem_arena &&
               !Settings::values.use_reactive_flushing.GetValue() &&
               Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce;
    }

    void RasterizerArmReadTraps(u64 vaddr, u64 size) {
        if (Settings::values.use_reactive_flushing.GetValue()) {
            // Clean cached read areas also need invalidation when reactive flushing is enabled.
            read_trap_generation.fetch_add(1, std::memory_order_release);
        }
        if (vaddr == 0 || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size) ||
            !ReadTrapsEnabled()) {
            return;
        }
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        std::scoped_lock lk{read_trap_mutex};
        u64 run_begin = 0;
        u64 run_pages = 0;
        const auto protect_run = [&] {
            if (run_pages > 0) {
                // Reads that hit a cached read area must not skip the flush of a new trap.
                read_trap_generation.fetch_add(1, std::memory_order_release);
                host_buffer->Protect(run_begin << YUZU_PAGEBITS, run_pages << YUZU_PAGEBITS,
                                     Common::MemoryPermission{});
                run_pages = 0;
            }
        };
        for (u64 page = vaddr >> YUZU_PAGEBITS; page != page_end; ++page) {
            const bool cached = std::as_const(current_page_table->entries)[page].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            if (!cached || !armed_read_traps.insert(page).second) {
                protect_run();
                continue;
            }
            armed_read_trap_count.fetch_add(1, std::memory_order_relaxed);
            if (run_pages == 0) {
                run_begin = page;
            }
            ++run_pages;
        }
        protect_run();
    }

    /// Takes read permission away again from armed pages that were just cached again.
    void ProtectArmedPages(u64 vaddr, u64 size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        for (auto it = armed_read_traps.lower_bound(vaddr >> YUZU_PAGEBITS);
             it != armed_read_traps.end() && *it < page_end; ++it) {
            host_buffer->Protect(*it << YUZU_PAGEBITS, YUZU_PAGESIZE, Common::MemoryPermission{});
        }
    }

    /// A slow-path read of a cached page: its trap, if any, has done its job.
    void DisarmReadTraps(u64 vaddr, size_t size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        auto it = armed_read_traps.lower_bound(vaddr >> YUZU_PAGEBITS);
        while (it != armed_read_traps.end() && *it < page_end) {
            // Still cached (this read came through the rasterizer path): writes stay trapped.
            host_buffer->Protect(*it << YUZU_PAGEBITS, YUZU_PAGESIZE,
                                 Common::MemoryPermission::Read);
            it = armed_read_traps.erase(it);
            armed_read_trap_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /**
     * Maps a region of pages as a specific type.
     *
     * @param page_table The page table to use to perform the mapping.
     * @param base       The base address to begin mapping at.
     * @param size       The total size of the range in bytes.
     * @param target     The target address to begin mapping from.
     * @param type       The page type to map the memory as.
     */
    void MapPages(Common::PageTable& page_table, Common::ProcessAddress base_address, u64 size,
                  Common::PhysicalAddress target, Common::PageType type) {
        auto base = GetInteger(base_address);

        LOG_DEBUG(HW_Memory, "Mapping {:016X} onto {:016X}-{:016X}", GetInteger(target),
                  base * YUZU_PAGESIZE, (base + size) * YUZU_PAGESIZE);

        const auto end = base + size;
        ASSERT_MSG(end <= page_table.entries.size(), "out of range mapping at {:016X}", base + page_table.entries.size());

        if (!target) {
            ASSERT_MSG(type != Common::PageType::Memory,
                       "Mapping memory page without a pointer @ {:016x}", base * YUZU_PAGESIZE);

            last_mapped_table = nullptr;
            page_table.entries.ZeroRegion(base, end);
        } else if (base != end) {
            const u64 first_host_ptr =
                reinterpret_cast<u64>(system.DeviceMemory().GetPointer<u8>(target)) -
                (base << YUZU_PAGEBITS);
            u16 current_block = Common::PageTable::PageEntryData::INVALID_BLOCK_ID;
            if (last_mapped_table == &page_table && base == last_mapped_end &&
                first_host_ptr == last_mapped_host_ptr &&
                last_mapped_block != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
                current_block = last_mapped_block;
                last_mapped_end = end;
            } else {
                u64 mapping_id = block_count.load(std::memory_order_relaxed);
                constexpr u64 exhausted_count =
                    u64{Common::PageTable::PageEntryData::INVALID_BLOCK_ID} + 1;
                while (mapping_id < exhausted_count &&
                       !block_count.compare_exchange_weak(mapping_id, mapping_id + 1,
                                                          std::memory_order_relaxed)) {
                }
                current_block =
                    Common::PageTable::PageEntryData::BlockIdForMapping(mapping_id);
                if (mapping_id == Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
                    LOG_WARNING(HW_Memory,
                                "Page table block ids are exhausted; validating spans page by page");
                }
                last_mapped_table = &page_table;
                last_mapped_end = end;
                last_mapped_host_ptr = first_host_ptr;
                last_mapped_block = current_block;
            }

            page_table.entries.CommitRegion(base, end);
            while (base != end) {
                auto host_ptr = reinterpret_cast<u64>(system.DeviceMemory().GetPointer<u8>(target)) - (base << YUZU_PAGEBITS);;
                auto& entry = page_table.entries.GetUnchecked(base);

                entry.Store(false, type, current_block, host_ptr);
                ASSERT_MSG(page_table.entries[base].Pointer(),
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                target += YUZU_PAGESIZE;
            }
        }
    }

    template<typename F, typename G>
    [[nodiscard]] u8* GetPointerImpl(u64 vaddr, F&& on_unmapped, G&& on_rasterizer) const {
        // AARCH64 masks the upper 16 bit of all memory accesses
        vaddr &= 0xffffffffffffULL;
        if (AddressSpaceContains(*current_page_table, vaddr, 1)) [[likely]] {
            // Avoid adding any extra logic to this fast-path block
            const auto raw = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Raw();
            if (auto pointer = Common::PageTable::PageEntryData::ExtractPointer(raw); pointer) [[likely]] {
                return reinterpret_cast<u8*>(pointer + vaddr);
            } else {
                switch (static_cast<Common::PageType>(raw.type)) {
                case Common::PageType::Memory:
                    ASSERT_MSG(false, "Mapped memory page without a pointer @ {:#016x}", vaddr);
                    return nullptr;
                case Common::PageType::DebugMemory:
                    return GetPointerFromDebugMemory(vaddr);
                case Common::PageType::RasterizerCachedMemory: {
                    u8* const host_ptr{GetPointerFromRasterizerCachedMemory(vaddr)};
                    on_rasterizer();
                    return host_ptr;
                }
                case Common::PageType::Unmapped: [[unlikely]] {
                    on_unmapped();
                    return nullptr;
                }
                default:
                    UNREACHABLE();
                }
                return nullptr;
            }
        } else {
            on_unmapped();
            return nullptr;
        }
    }

    [[nodiscard]] u8* GetPointer(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr),
            [vaddr]() {
                LOG_ERROR(HW_Memory, "Unmapped GetPointer @ {:#016x}", GetInteger(vaddr));
            },
            []() {});
    }

    [[nodiscard]] u8* GetPointerSilent(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr), []() {}, []() {});
    }

    [[nodiscard]] u8* GetContiguousSpan(VAddr addr, std::size_t size) const noexcept {
        u8* const base = GetPointerSilent(addr);
        if (base == nullptr) {
            return nullptr;
        }

        const VAddr end = addr + size;
        const VAddr first_page = ((addr >> YUZU_PAGEBITS) + 1) << YUZU_PAGEBITS;
        const std::uintptr_t base_ptr = reinterpret_cast<std::uintptr_t>(base);
        for (VAddr page = first_page; page < end; page += YUZU_PAGESIZE) {
            const std::uintptr_t expected = base_ptr + (page - addr);
            if (reinterpret_cast<std::uintptr_t>(GetPointerSilent(page)) != expected) {
                return nullptr;
            }
        }
        return base;
    }

    /// @brief Reads a particular data type out of memory at the given virtual address.
    /// @param vaddr The virtual address to read the data type from.
    /// @tparam T The data type to read out of memory.
    /// @returns The instance of T read from the specified virtual address.
    template <typename T>
    inline T Read(Common::ProcessAddress vaddr) noexcept requires(std::is_trivially_copyable_v<T>) {
        const u64 addr = GetInteger(vaddr);
        if (auto const ptr = GetPointerImpl(addr, [addr]() {
            // A guest spinning on a bad pointer reads unmapped memory tens of thousands of times
            // a second: keep the first ones and then a sample.
            static std::atomic<u64> unmapped_reads{};
            if (const u64 count = unmapped_reads.fetch_add(1); count < 1000 || count % 100000 == 0) {
                LOG_ERROR(HW_Memory, "Unmapped Read{} @ {:#016x} (#{})", sizeof(T) * 8, addr, count);
            }
            ++UnmappedReadFlag();
        }, [&]() {
            HandleRasterizerDownload(addr, sizeof(T));
        }); ptr) [[likely]] {
            // It may be tempting to rewrite this particular section to use "reinterpret_cast";
            // afterall, it's trivially copyable so surely it can be copied ov- Alignment.
            // Remember, alignment. memcpy() will deal with all the alignment extremely fast.
            T result{};
            std::memcpy(&result, ptr, sizeof(T));
            return result;
        }
        return T{};
    }

    /// @brief Writes a particular data type to memory at the given virtual address.
    /// @param vaddr The virtual address to write the data type to.
    /// @tparam T The data type to write to memory.
    template <typename T>
    inline void Write(Common::ProcessAddress vaddr, const T data) noexcept requires(std::is_trivially_copyable_v<T>) {
        const u64 addr = GetInteger(vaddr);
        if (war_probe) [[unlikely]] {
            WarOnWrite(addr, sizeof(T));
        }
        if (auto const ptr = GetPointerImpl(addr, [addr, data]() {
            LOG_ERROR(HW_Memory, "Unmapped Write{} @ {:#016x} = {:#016x}", sizeof(T) * 8, addr, u64(data));
        }, [&]() { HandleRasterizerWrite(addr, sizeof(T)); }); ptr) [[likely]]
            std::memcpy(ptr, &data, sizeof(T));
    }

    template <typename T>
    bool WriteExclusive(Common::ProcessAddress vaddr, const T data, const T expected) {
        if (war_probe) [[unlikely]] {
            WarOnWrite(GetInteger(vaddr), sizeof(T));
        }
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive{} @ {:#016x} = {:#016x}",
                          sizeof(T) * 8, GetInteger(vaddr), static_cast<u64>(data));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(T)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<T*>(ptr), data, expected);
        }
        return true;
    }

    bool WriteExclusive128(Common::ProcessAddress vaddr, const u128 data, const u128 expected) {
        if (war_probe) [[unlikely]] {
            WarOnWrite(GetInteger(vaddr), sizeof(u128));
        }
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive128 @ {:#016x} = {:#016x}{:016X}",
                          GetInteger(vaddr), static_cast<u64>(data[1]), static_cast<u64>(data[0]));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(u128)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<u64*>(ptr), data, expected);
        }
        return true;
    }

    void HandleRasterizerDownload(VAddr v_address, size_t size) {
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        const size_t core = system.GetCurrentHostThreadID();
        if (core >= Core::Hardware::NUM_CPU_CORES) {
            // A service thread has no slot in the arrays below, and it does not use a slot of a
            // core. It keeps no area, so it asks on each read.
            if (static std::atomic_flag logged; !logged.test_and_set(std::memory_order_relaxed)) {
                LOG_INFO(HW_Memory, "Read of cached guest memory from a thread that is not a core");
            }
            Common::ScratchBuffer<u32> scratch;
            gpu_device_memory->ApplyOpOnPointer(p, scratch, [&](DAddr address) {
                if (war_probe) [[unlikely]] {
                    WarNoteTracked(v_address, VideoCommon::Nx2WarProbe::Category::Read);
                }
                DisarmReadTraps(v_address, size);
                if (war_inflight) [[unlikely]] {
                    VideoCommon::Nx2Inflight::read_note = {};
                }
                (void)system.GPU().OnCPURead(address, size);
                if (war_inflight) [[unlikely]] {
                    InflightAfterRead(p, size, false);
                }
            });
            return;
        }
        auto& current_area = rasterizer_read_areas[core];
        if (const u64 generation = read_trap_generation.load(std::memory_order_acquire);
            read_area_generations[core] != generation) {
            current_area = {};
            read_area_generations[core] = generation;
        }
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            const DAddr end_address = address + size;
            if (current_area.start_address <= address && end_address <= current_area.end_address)
                [[likely]] {
                return;
            }
            if (war_probe) [[unlikely]] {
                WarNoteTracked(v_address, VideoCommon::Nx2WarProbe::Category::Read);
            }
            DisarmReadTraps(v_address, size);
            if (war_inflight) [[unlikely]] {
                VideoCommon::Nx2Inflight::read_note = {};
            }
            current_area = system.GPU().OnCPURead(address, size);
            if (war_inflight) [[unlikely]] {
                InflightAfterRead(p, size, true);
            }
        });
    }

    void HandleRasterizerWrite(VAddr v_address, size_t size) {
        if (war_inflight) [[unlikely]] {
            InflightBeforeWrite();
        }
        if (war_probe) [[unlikely]] {
            WarNoteTracked(v_address, VideoCommon::Nx2WarProbe::Category::Cached);
        }
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            auto& current_area = rasterizer_write_areas[core];
            PAddr subaddress = address >> YUZU_PAGEBITS;
            // Performance note:
            // It may not be a good idea to assume accesses are within the same subaddress (i.e same page)
            // It is often the case the games like to access wildly different addresses. Hence why I propose
            // we should let the compiler just do it's thing...
            if (current_area.last_address != subaddress) {
                // Short circuit the need to check for address/size
                auto const do_collection = (address != 0 && size != 0)
                    && system.GPU().OnCPUWrite(address, size);
                if (do_collection) {
                    current_area.last_address = subaddress;
                } else {
                    return;
                }
            }
            gpu_dirty_managers[core].Collect(address, size);
        });
    }

    struct GPUDirtyState {
        PAddr last_address;
    };

    // Nx2Perf war_guard_probe (video_core/nx2_war_probe.h). The tracker leaves a GPU page read/
    // write between a CPU write and its next upload (RasterizerMarkRegionCached(false)). After
    // vi2's reply the probe write-protects those pages (host Read only; page type DebugMemory, so
    // the JIT's page table path takes the callbacks too) and gives each one its tracker state back
    // (Memory, read/write) on its first write. The bytes and order of accesses do not change.

    /// Page numbers in a fixed-capacity hash set with a dense list (no allocation after Init).
    struct WarPageSet {
        static constexpr u32 SlotBits = 18;
        static constexpr u32 Slots = 1U << SlotBits;
        static constexpr u32 MaxEntries = Slots / 2;
        std::unique_ptr<u32[]> slot_key;   ///< page + 1, 0 when empty
        std::unique_ptr<u32[]> slot_index; ///< Index of the page in list
        std::unique_ptr<u32[]> list;
        std::unique_ptr<u32[]> scratch;    ///< Arming: pages to protect, sorted into runs
        u32 count = 0;
        u64 overflow = 0;

        void Init() {
            if (!slot_key) {
                slot_key = std::make_unique<u32[]>(Slots);
                slot_index = std::make_unique<u32[]>(Slots);
                list = std::make_unique<u32[]>(MaxEntries);
                scratch = std::make_unique<u32[]>(MaxEntries);
            }
        }
        static u32 Home(u32 key) {
            return (key * 0x9E3779B1U) >> (32 - SlotBits);
        }
        u32 Find(u64 page) const {
            if (count == 0) {
                return Slots;
            }
            const u32 key = static_cast<u32>(page) + 1;
            for (u32 slot = Home(key);; slot = (slot + 1) & (Slots - 1)) {
                if (slot_key[slot] == key) {
                    return slot;
                }
                if (slot_key[slot] == 0) {
                    return Slots;
                }
            }
        }
        bool Contains(u64 page) const {
            return Find(page) != Slots;
        }
        void Insert(u64 page) {
            const u32 key = static_cast<u32>(page) + 1;
            u32 slot = Home(key);
            for (; slot_key[slot] != 0; slot = (slot + 1) & (Slots - 1)) {
                if (slot_key[slot] == key) {
                    return;
                }
            }
            if (count == MaxEntries) {
                ++overflow;
                return;
            }
            slot_key[slot] = key;
            slot_index[slot] = count;
            list[count++] = static_cast<u32>(page);
        }
        void Erase(u64 page) {
            const u32 slot = Find(page);
            if (slot == Slots) {
                return;
            }
            const u32 index = slot_index[slot];
            const u32 last = list[count - 1];
            if (index != count - 1) {
                list[index] = last;
                slot_index[Find(last)] = index;
            }
            --count;
            // Backward-shift deletion keeps every probe sequence intact.
            u32 hole = slot;
            for (u32 next = (hole + 1) & (Slots - 1); slot_key[next] != 0;
                 next = (next + 1) & (Slots - 1)) {
                const u32 home = Home(slot_key[next]);
                if (((next - home) & (Slots - 1)) >= ((next - hole) & (Slots - 1))) {
                    slot_key[hole] = slot_key[next];
                    slot_index[hole] = slot_index[next];
                    hole = next;
                }
            }
            slot_key[hole] = 0;
        }
    };

    bool WarGuestCore() const {
        return system.GetCurrentHostThreadID() < Core::Hardware::NUM_CPU_CORES;
    }

    bool WarIsBankPage(u64 page) const {
        return std::binary_search(war_bank_pages.begin(), war_bank_pages.begin() + war_bank_count,
                                  page);
    }

    template <typename... Args>
    void WarVerifyDifference(fmt::format_string<Args...> format, Args&&... args) {
        const u64 count = VideoCommon::Nx2WarProbe::Get().verify_differences.fetch_add(
                              1, std::memory_order_relaxed) +
                          1;
        if (count <= 64) {
            LOG_ERROR(HW_GPU, "Nx2Perf war_guard_probe: {} ({} differences so far)",
                      fmt::format(format, std::forward<Args>(args)...), count);
        }
    }

    /// Gives a page the probe guarded its tracker state back: Memory, read/write. Caller holds
    /// war_mutex; the page is in war_unprotected.
    bool WarUnguard(u64 page) {
        auto& entry = current_page_table->entries.GetUnchecked(page);
        const auto [pointer, type, block] = entry.PointerTypeBlock(true);
        if (type != Common::PageType::DebugMemory) {
            return false;
        }
        entry.Store(false, Common::PageType::Memory, block, pointer);
        host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                             Common::MemoryPermission::ReadWrite);
        return true;
    }

    /// The kernel or the debugger is about to change [vaddr, vaddr + size): the probe lets go of
    /// those pages first. Caller holds war_mutex.
    void WarForget(u64 vaddr, u64 size) {
        if (war_unprotected.count == 0 || size == 0) {
            return;
        }
        const u64 first = vaddr >> YUZU_PAGEBITS;
        const u64 end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        if (end - first <= war_unprotected.count) {
            for (u64 page = first; page != end; ++page) {
                if (war_unprotected.Contains(page)) {
                    WarUnguard(page);
                    war_unprotected.Erase(page);
                }
            }
            return;
        }
        for (u32 i = war_unprotected.count; i-- > 0;) {
            const u64 page = war_unprotected.list[i];
            if (page >= first && page < end) {
                WarUnguard(page);
                war_unprotected.Erase(page);
            }
        }
    }

    /// RasterizerMarkRegionCached has set the tracker's protection (caller holds war_mutex).
    void WarTrackerMarked(u64 vaddr, u64 size, bool cached) {
        const u64 first = vaddr >> YUZU_PAGEBITS;
        const u64 end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        for (u64 page = first; page != end; ++page) {
            if (cached) {
                // MarkRasterizerCached already replaced a guard's page type.
                war_unprotected.Erase(page);
                continue;
            }
            auto& entry = current_page_table->entries.GetUnchecked(page);
            auto [pointer, type, block] = entry.PointerTypeBlock(true);
            if (type == Common::PageType::DebugMemory && war_unprotected.Contains(page)) {
                // Read/write is set already; an uncached page's type is Memory.
                entry.Store(false, Common::PageType::Memory, block, pointer);
                type = Common::PageType::Memory;
            }
            if (type == Common::PageType::Memory) {
                war_unprotected.Insert(page);
            }
        }
    }

    /// A slow-path write: the first one to a guarded page gives the page its permission back.
    void WarOnWrite(u64 vaddr, size_t size) {
        vaddr &= 0xffffffffffffULL;
        if (size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }
        const u64 end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        for (u64 page = vaddr >> YUZU_PAGEBITS; page != end; ++page) {
            if (std::as_const(current_page_table->entries)[page].Type() !=
                Common::PageType::DebugMemory) [[likely]] {
                continue;
            }
            if (war_inflight) [[unlikely]] {
                InflightBeforeWrite();
            }
            std::scoped_lock lk{war_mutex};
            if (!war_unprotected.Contains(page) || !WarUnguard(page)) {
                continue; // A debugger's page, or given back meanwhile
            }
            using VideoCommon::Nx2WarProbe::Category;
            VideoCommon::Nx2WarProbe::NoteTrap(
                WarIsBankPage(page) ? Category::Bank : Category::Unprotected, WarGuestCore());
            if (VideoCommon::Nx2Perf::Verify()) {
                const auto [pointer, type, block] =
                    std::as_const(current_page_table->entries)[page].PointerTypeBlock(true);
                if (type != Common::PageType::Memory || pointer == 0) {
                    WarVerifyDifference("page {:#x} is type {} after its trap gave it back, the "
                                        "tracker wants an uncached page",
                                        page << YUZU_PAGEBITS, static_cast<u32>(type));
                }
            }
        }
    }

    /// A write (Cached) or read (Read) trap of a page the tracker protects.
    void WarNoteTracked(u64 vaddr, VideoCommon::Nx2WarProbe::Category category) {
        using VideoCommon::Nx2WarProbe::Category;
        const u64 page = (vaddr & 0xffffffffffffULL) >> YUZU_PAGEBITS;
        bool bank = false;
        if (category == Category::Cached) {
            std::scoped_lock lk{war_mutex};
            bank = WarIsBankPage(page);
        }
        VideoCommon::Nx2WarProbe::NoteTrap(bank ? Category::Bank : category, WarGuestCore());
    }

    /// set_banks false (an inflight epoch's arm) keeps the probe's bank page labels.
    Nx2WarProbeArmResult WarArm(std::span<const u64> bank_cpu_pages, bool set_banks = true) {
        Nx2WarProbeArmResult result{};
        if (!war_probe) {
            return result;
        }
        std::scoped_lock lk{war_mutex};
        if (set_banks) {
            war_bank_count = (std::min)(bank_cpu_pages.size(), war_bank_pages.size());
            std::copy_n(bank_cpu_pages.begin(), war_bank_count, war_bank_pages.begin());
            std::sort(war_bank_pages.begin(), war_bank_pages.begin() + war_bank_count);
            war_bank_count = static_cast<size_t>(
                std::unique(war_bank_pages.begin(), war_bank_pages.begin() + war_bank_count) -
                war_bank_pages.begin());
        }

        u32 to_protect = 0;
        for (u32 i = war_unprotected.count; i-- > 0;) {
            const u64 page = war_unprotected.list[i];
            auto& entry = current_page_table->entries.GetUnchecked(page);
            const auto [pointer, type, block] = entry.PointerTypeBlock(true);
            if (type == Common::PageType::DebugMemory) {
                ++result.guarded;
                continue;
            }
            if (type != Common::PageType::Memory || pointer == 0) {
                if (VideoCommon::Nx2Perf::Verify()) {
                    WarVerifyDifference("page {:#x} the tracker left writable is type {}",
                                        page << YUZU_PAGEBITS, static_cast<u32>(type));
                }
                war_unprotected.Erase(page);
                continue;
            }
            entry.Store(true, Common::PageType::DebugMemory, block, pointer);
            war_unprotected.scratch[to_protect++] = static_cast<u32>(page);
        }
        u32* const pages = war_unprotected.scratch.get();
        std::sort(pages, pages + to_protect);
        for (u32 i = 0; i < to_protect;) {
            u32 run = 1;
            while (i + run < to_protect && pages[i + run] == pages[i] + run) {
                ++run;
            }
            host_buffer->Protect(u64{pages[i]} << YUZU_PAGEBITS, u64{run} << YUZU_PAGEBITS,
                                 Common::MemoryPermission::Read);
            i += run;
        }
        result.armed = to_protect;
        result.unprotected = war_unprotected.count;
        result.overflow = war_unprotected.overflow;
        for (size_t i = 0; i < war_bank_count; ++i) {
            const u64 page = war_bank_pages[i];
            if (page == 0 ||
                !AddressSpaceContains(*current_page_table, page << YUZU_PAGEBITS, 1)) {
                ++result.bank_unguardable;
            } else if (war_unprotected.Contains(page)) {
                ++result.bank_guarded;
            } else if (std::as_const(current_page_table->entries)[page].Type() ==
                       Common::PageType::RasterizerCachedMemory) {
                ++result.bank_tracked;
            } else {
                ++result.bank_unguardable;
            }
        }
        return result;
    }

    // Nx2Perf inflight_write_wait / inflight_read_exact (video_core/nx2_war_probe.h, Nx2Inflight).

    /// Gives every page an epoch's arm guarded its tracker state back (Memory, read/write).
    void InflightUnguardAll() {
        std::scoped_lock lk{war_mutex};
        auto& epoch = VideoCommon::Nx2Inflight::Get();
        if (!epoch.guarded.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        u32 count = 0;
        for (u32 i = 0; i < war_unprotected.count; ++i) {
            const u64 page = war_unprotected.list[i];
            auto& entry = current_page_table->entries.GetUnchecked(page);
            const auto [pointer, type, block] = entry.PointerTypeBlock(true);
            if (type != Common::PageType::DebugMemory) {
                continue;
            }
            entry.Store(false, Common::PageType::Memory, block, pointer);
            war_unprotected.scratch[count++] = static_cast<u32>(page);
        }
        u32* const pages = war_unprotected.scratch.get();
        std::sort(pages, pages + count);
        for (u32 i = 0; i < count;) {
            u32 run = 1;
            while (i + run < count && pages[i + run] == pages[i] + run) {
                ++run;
            }
            host_buffer->Protect(u64{pages[i]} << YUZU_PAGEBITS, u64{run} << YUZU_PAGEBITS,
                                 Common::MemoryPermission::ReadWrite);
            i += run;
        }
    }

    /// Waits until syncpoint `id` (its host value with `host`, else its guest value) reaches
    /// `value`. Gives up (returns false) after 4 s, or 3 ms after a GPU channel was seen suspended
    /// on a semaphore, which this thread may be the one to release.
    bool InflightWait(u32 id, u32 value, bool host) {
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto ready = [&] {
            return host ? syncpoints.IsReadyHost(id, value) : syncpoints.IsReadyGuest(id, value);
        };
        if (id >= 192 || ready()) {
            return true;
        }
        struct Wake {
            std::mutex mutex;
            std::condition_variable cv;
            bool done{};
        };
        auto wake = std::make_shared<Wake>();
        const auto notify = [wake] {
            {
                std::scoped_lock lk{wake->mutex};
                wake->done = true;
            }
            wake->cv.notify_all();
        };
        if (host) {
            syncpoints.RegisterHostAction(id, value, notify);
        } else {
            syncpoints.RegisterGuestAction(id, value, notify);
        }
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds{4};
        bool gpu_waited = false;
        std::unique_lock lk{wake->mutex};
        while (!wake->done && system.IsPoweredOn()) {
            const auto now = std::chrono::steady_clock::now();
            gpu_waited |= system.GPU().HasSuspendedChannels();
            if (now >= deadline || (gpu_waited && now - start > std::chrono::milliseconds{3})) {
                break;
            }
            wake->cv.wait_for(lk, std::chrono::milliseconds{1});
        }
        lk.unlock();
        return ready();
    }

    template <typename... Args>
    void InflightLogEscape(fmt::format_string<Args...> format, Args&&... args) {
        auto& epoch = VideoCommon::Nx2Inflight::Get();
        if (epoch.escape_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_WARNING(HW_GPU, "Nx2Perf inflight_write_wait: {}",
                        fmt::format(format, std::forward<Args>(args)...));
        }
    }

    /// A guest write of a page the back may read for the open epoch's frame: on a guest core,
    /// waits for the epoch's fence and closes the epoch, so this and every later write land after
    /// the back passed it. A closed epoch's leftover guards are dropped at the first write.
    void InflightBeforeWrite() {
        namespace Inflight = VideoCommon::Nx2Inflight;
        Inflight::Epoch& epoch = Inflight::Get();
        const u64 seq = epoch.open_seq.load(std::memory_order_acquire);
        if (seq == 0) {
            if (epoch.guarded.load(std::memory_order_relaxed)) [[unlikely]] {
                InflightUnguardAll();
            }
            return;
        }
        if (!Inflight::WriteWait()) {
            return; // inflight_read_exact alone: the epoch only serves the reads
        }
        if (!WarGuestCore()) {
            // Service threads never wait: they may be what the back is waiting for.
            epoch.host_writes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const u32 id = epoch.fence_id.load(std::memory_order_relaxed);
        const u32 value = epoch.fence_value.load(std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        const bool reached = InflightWait(id, value, false);
        const u64 waited = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - start)
                                                .count());
        u64 expected = seq;
        if (epoch.open_seq.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            if (reached) {
                epoch.closed_by_write.fetch_add(1, std::memory_order_relaxed);
            } else {
                epoch.escapes.fetch_add(1, std::memory_order_relaxed);
                InflightLogEscape("epoch {} write gave up on fence ({}, {}) after {} us (now {}, "
                                  "{}): the write lands before the back passed it",
                                  seq, id, value, waited / 1000,
                                  system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(id),
                                  system.GPU().HasSuspendedChannels() ? "a GPU channel waits"
                                                                      : "4 s timeout");
            }
        }
        epoch.write_waits.fetch_add(1, std::memory_order_relaxed);
        epoch.write_wait_ns.fetch_add(waited, std::memory_order_relaxed);
        u64 max = epoch.max_wait_ns.load(std::memory_order_relaxed);
        while (waited > max &&
               !epoch.max_wait_ns.compare_exchange_weak(max, waited, std::memory_order_relaxed)) {
        }
        InflightUnguardAll();
    }

    /// After OnCPURead: a read the buffer cache served from a preflush mark while an epoch was
    /// open. inflight_read_exact waits (guest cores only) for the in-flight writer's host fence,
    /// or for the previous fence's when that has not landed; Verify compares the reads served
    /// without a wait with their bytes after fence N lands.
    void InflightAfterRead(const u8* host, size_t size, bool guest_core) {
        namespace Inflight = VideoCommon::Nx2Inflight;
        const Inflight::ReadNote note =
            std::exchange(Inflight::read_note, Inflight::ReadNote{});
        if (!note.preflush_served) {
            return;
        }
        Inflight::Epoch& epoch = Inflight::Get();
        const u64 seq = epoch.open_seq.load(std::memory_order_acquire);
        if (seq == 0) {
            return;
        }
        epoch.preflush_reads.fetch_add(1, std::memory_order_relaxed);
        const u32 id = epoch.fence_id.load(std::memory_order_relaxed);
        const u32 value = epoch.fence_value.load(std::memory_order_relaxed);
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (Inflight::ReadExact() && guest_core) {
            u32 wait_id = ~0U;
            u32 wait_value = 0;
            const u32 prev_id = epoch.prev_id.load(std::memory_order_relaxed);
            const u32 prev_value = epoch.prev_value.load(std::memory_order_relaxed);
            if (note.in_flight) {
                wait_id = id;
                wait_value = value;
            } else if (prev_id < 192 && !syncpoints.IsReadyHost(prev_id, prev_value)) {
                wait_id = prev_id;
                wait_value = prev_value;
            }
            if (wait_id < 192 && !syncpoints.IsReadyHost(wait_id, wait_value)) {
                const auto start = std::chrono::steady_clock::now();
                const bool landed = InflightWait(wait_id, wait_value, true);
                epoch.read_waits.fetch_add(1, std::memory_order_relaxed);
                epoch.read_wait_ns.fetch_add(
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count()),
                    std::memory_order_relaxed);
                if (landed) {
                    return; // Exact: everything up to the writer's fence has landed.
                }
                epoch.read_escapes.fetch_add(1, std::memory_order_relaxed);
                InflightLogEscape("epoch {} read gave up on the host fence ({}, {}) (host {}): "
                                  "it reads the preflushed bytes",
                                  seq, wait_id, wait_value,
                                  syncpoints.GetHostSyncpointValue(wait_id));
            }
        }
        if (!VideoCommon::Nx2Perf::Verify() || host == nullptr || id >= 192 ||
            syncpoints.IsReadyHost(id, value)) {
            return;
        }
        // Verify (b): the bytes this read sees now, compared when fence N lands.
        auto& pool = Inflight::ReadCompares();
        Inflight::ReadCompare* slot = nullptr;
        for (Inflight::ReadCompare& candidate : pool) {
            u32 free = 0;
            if (candidate.busy.compare_exchange_strong(free, 1, std::memory_order_acquire)) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            epoch.read_compare_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const size_t in_page =
            YUZU_PAGESIZE - (reinterpret_cast<uintptr_t>(host) & YUZU_PAGEMASK);
        slot->size =
            static_cast<u32>((std::min)({size, Inflight::ReadCompare::MaxBytes, in_page}));
        slot->host = host;
        slot->address = reinterpret_cast<uintptr_t>(host);
        std::memcpy(slot->bytes.data(), host, slot->size);
        syncpoints.RegisterHostAction(id, value, [slot, seq, in_flight = note.in_flight] {
            Inflight::Epoch& e = Inflight::Get();
            e.read_compared.fetch_add(1, std::memory_order_relaxed);
            if (std::memcmp(slot->bytes.data(), slot->host, slot->size) != 0) {
                const u64 count = e.read_differs.fetch_add(1, std::memory_order_relaxed) + 1;
                if (e.verify_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf inflight_read_exact: differs, a {}-byte read at host {:#x} "
                              "in epoch {} ({} block) changed when its fence landed ({} so far)",
                              slot->size, slot->address, seq,
                              in_flight ? "an in-flight" : "a settled", count);
                }
            }
            slot->busy.store(0, std::memory_order_release);
        });
    }

    /// vi2's early reply to the present of fence (id, value), after waiting for (prev_id,
    /// prev_value): opens the epoch and arms the guards. False when none can open (switch off,
    /// or the page set lost pages): vi2 then waits for this frame itself.
    bool InflightOpen(u32 id, u32 value, u32 prev_id, u32 prev_value) {
        namespace Inflight = VideoCommon::Nx2Inflight;
        if (!war_inflight || id >= 192) {
            return false;
        }
        Inflight::Epoch& epoch = Inflight::Get();
        if (epoch.open_seq.exchange(0, std::memory_order_acq_rel) != 0) {
            epoch.closed_by_present.fetch_add(1, std::memory_order_relaxed);
        }
        const u64 seq = epoch.seq.fetch_add(1, std::memory_order_relaxed) + 1;
        // Writes stamped after the back passed the previous fence belong to this frame. That
        // fence's epoch recorded the stamp as it passed; without one, every written block counts.
        u64 prev_stamp = 0;
        if (epoch.passed_seq.load(std::memory_order_acquire) == seq - 1 &&
            epoch.fence_id.load(std::memory_order_relaxed) == prev_id &&
            epoch.fence_value.load(std::memory_order_relaxed) == prev_value) {
            prev_stamp = epoch.passed_stamp.load(std::memory_order_relaxed);
        }
        epoch.prev_fence_stamp.store(prev_stamp, std::memory_order_relaxed);
        epoch.fence_id.store(id, std::memory_order_relaxed);
        epoch.fence_value.store(value, std::memory_order_relaxed);
        epoch.prev_id.store(prev_id, std::memory_order_relaxed);
        epoch.prev_value.store(prev_value, std::memory_order_relaxed);
        // Open before arming: a write that traps meanwhile waits, and its close unguards after
        // the arm (both take war_mutex).
        epoch.guarded.store(true, std::memory_order_release);
        epoch.open_seq.store(seq, std::memory_order_release);
        // The full list of pages the tracker leaves writable; tracker-protected pages trap
        // already (HandleRasterizerWrite). A page the set could not hold would write unguarded.
        const auto result = WarArm({}, false);
        if (result.overflow != 0) {
            u64 expected = seq;
            epoch.open_seq.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
            InflightUnguardAll();
            epoch.failed_opens.fetch_add(1, std::memory_order_relaxed);
            if (static std::atomic_flag logged; !logged.test_and_set()) {
                LOG_WARNING(HW_GPU,
                            "Nx2Perf inflight_write_wait: the guard set overflowed ({} pages), "
                            "presents wait for their own fence from now on",
                            result.overflow);
            }
            return false;
        }
        if (VideoCommon::Nx2WarProbe::On()) {
            auto& probe = VideoCommon::Nx2WarProbe::Get();
            std::scoped_lock lk{probe.snapshot_mutex};
            probe.snapshot_epoch = seq;
        }
        epoch.opened.fetch_add(1, std::memory_order_relaxed);
        system.Host1x().GetSyncpointManager().RegisterGuestAction(id, value, [seq] {
            Inflight::Epoch& e = Inflight::Get();
            e.passed_stamp.store(e.write_stamp.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            e.passed_seq.store(seq, std::memory_order_release);
            u64 current = e.fence_reached_seq.load(std::memory_order_relaxed);
            while (current < seq && !e.fence_reached_seq.compare_exchange_weak(
                                        current, seq, std::memory_order_acq_rel)) {
            }
            u64 expected = seq;
            if (e.open_seq.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                e.closed_by_fence.fetch_add(1, std::memory_order_relaxed);
            }
        });
        return true;
    }

    void InvalidateGPUMemory(u8* p, size_t size) {
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        auto& gpu = system.GPU();
        gpu_device_memory->ApplyOpOnPointer(
            p, scratch_buffers[core], [&](DAddr address) { gpu.InvalidateRegion(address, size, true); });
    }

    Core::System& system;
    Tegra::MaxwellDeviceMemoryManager* gpu_device_memory{};
    Common::PageTable* current_page_table = nullptr;

    std::array<VideoCore::RasterizerDownloadArea, Core::Hardware::NUM_CPU_CORES> rasterizer_read_areas{};
    std::array<u64, Core::Hardware::NUM_CPU_CORES> read_area_generations{};
    std::atomic<u64> read_trap_generation{};
    std::set<u64> armed_read_traps;
    std::atomic<size_t> armed_read_trap_count{};
    std::mutex read_trap_mutex;
    std::array<GPUDirtyState, Core::Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::array<Common::ScratchBuffer<u32>, Core::Hardware::NUM_CPU_CORES> scratch_buffers{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;
    bool war_probe = false; ///< war_guard_probe or the inflight epochs, application with fastmem
    bool war_inflight = false; ///< inflight_write_wait / inflight_read_exact can open epochs
    std::mutex war_mutex;
    WarPageSet war_unprotected;
    std::array<u64, VideoCommon::Nx2WarProbe::MaxBankPages> war_bank_pages{};
    size_t war_bank_count = 0;
#ifdef __ANDROID__
    std::optional<Common::HeapTracker> heap_tracker;
    Common::HeapTracker* host_buffer{};
#else
    Common::HostMemory* host_buffer{};
#endif
    std::atomic<u64> block_count = 0;
    const Common::PageTable* last_mapped_table{};
    u64 last_mapped_end{};
    u64 last_mapped_host_ptr{};
    u16 last_mapped_block{Common::PageTable::PageEntryData::INVALID_BLOCK_ID};
};

Memory::Memory(Core::System& system_) : system{system_} {
    Reset();
}

Memory::~Memory() = default;

void Memory::Reset() {
    impl = std::make_unique<Impl>(system);
}

void Memory::SetCurrentPageTable(Kernel::KProcess& process) {
    impl->SetCurrentPageTable(process);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                             Common::PhysicalAddress target, Common::MemoryPermission perms,
                             bool separate_heap) {
    impl->MapMemoryRegion(page_table, base, size, target, perms, separate_heap);
}

void Memory::UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         bool separate_heap) {
    impl->UnmapRegion(page_table, base, size, separate_heap);
}

void Memory::ProtectRegion(Common::PageTable& page_table, Common::ProcessAddress vaddr, u64 size,
                           Common::MemoryPermission perms) {
    impl->ProtectRegion(page_table, GetInteger(vaddr), size, perms);
}

bool Memory::IsValidVirtualAddress(const Common::ProcessAddress vaddr) const {
    const auto& page_table = *impl->current_page_table;
    const size_t page = vaddr >> YUZU_PAGEBITS;
    if (page >= page_table.entries.size()) {
        return false;
    }
    const auto [pointer, type, _] = page_table.entries[page].PointerTypeBlock();
    return pointer != 0 || type == Common::PageType::RasterizerCachedMemory ||
           type == Common::PageType::DebugMemory;
}

bool Memory::IsValidVirtualAddressRange(Common::ProcessAddress base, u64 size) const {
    Common::ProcessAddress end = base + size;
    Common::ProcessAddress page = Common::AlignDown(GetInteger(base), YUZU_PAGESIZE);

    for (; page < end; page += YUZU_PAGESIZE) {
        if (!IsValidVirtualAddress(page)) {
            return false;
        }
    }

    return true;
}

u8* Memory::GetPointer(Common::ProcessAddress vaddr) {
    return impl->GetPointer(vaddr);
}

u8* Memory::GetPointerSilent(Common::ProcessAddress vaddr) {
    return impl->GetPointerSilent(vaddr);
}

const u8* Memory::GetPointer(Common::ProcessAddress vaddr) const {
    return impl->GetPointer(vaddr);
}

u8 Memory::Read8(const Common::ProcessAddress addr) {
    return impl->Read8(addr);
}

u16 Memory::Read16(const Common::ProcessAddress addr) {
    return impl->Read16(addr);
}

u32 Memory::Read32(const Common::ProcessAddress addr) {
    return impl->Read32(addr);
}

u64 Memory::Read64(const Common::ProcessAddress addr) {
    return impl->Read64(addr);
}

void Memory::Write8(Common::ProcessAddress addr, u8 data) {
    impl->Write8(addr, data);
}

void Memory::Write16(Common::ProcessAddress addr, u16 data) {
    impl->Write16(addr, data);
}

void Memory::Write32(Common::ProcessAddress addr, u32 data) {
    impl->Write32(addr, data);
}

void Memory::Write64(Common::ProcessAddress addr, u64 data) {
    impl->Write64(addr, data);
}

bool Memory::WriteExclusive8(Common::ProcessAddress addr, u8 data, u8 expected) {
    return impl->WriteExclusive8(addr, data, expected);
}

bool Memory::WriteExclusive16(Common::ProcessAddress addr, u16 data, u16 expected) {
    return impl->WriteExclusive16(addr, data, expected);
}

bool Memory::WriteExclusive32(Common::ProcessAddress addr, u32 data, u32 expected) {
    return impl->WriteExclusive32(addr, data, expected);
}

bool Memory::WriteExclusive64(Common::ProcessAddress addr, u64 data, u64 expected) {
    return impl->WriteExclusive64(addr, data, expected);
}

bool Memory::WriteExclusive128(Common::ProcessAddress addr, u128 data, u128 expected) {
    return impl->WriteExclusive128(addr, data, expected);
}

std::string Memory::ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
    return impl->ReadCString(vaddr, max_length);
}

bool Memory::ReadBlock(const Common::ProcessAddress src_addr, void* dest_buffer,
                       const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, false);
}

bool Memory::ReadBlockUnsafe(const Common::ProcessAddress src_addr, void* dest_buffer,
                             const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, true);
}

const u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) const {
    return impl->GetSpan(src_addr, size);
}

u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) {
    return impl->GetSpan(src_addr, size);
}

bool Memory::WriteBlock(const Common::ProcessAddress dest_addr, const void* src_buffer,
                        const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, false);
}

bool Memory::WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void* src_buffer,
                              const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, true);
}

bool Memory::CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                       const std::size_t size) {
    return impl->CopyBlock(dest_addr, src_addr, size);
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->ZeroBlock(dest_addr, size);
}

void Memory::SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers) {
    impl->gpu_dirty_managers = managers;
}

Result Memory::InvalidateDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->InvalidateDataCache(dest_addr, size);
}

Result Memory::StoreDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->StoreDataCache(dest_addr, size);
}

Result Memory::FlushDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->FlushDataCache(dest_addr, size);
}

void Memory::RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached) {
    impl->RasterizerMarkRegionCached(GetInteger(vaddr), size, cached);
}

void Memory::RasterizerArmReadTraps(Common::ProcessAddress vaddr, u64 size) {
    impl->RasterizerArmReadTraps(GetInteger(vaddr), size);
}

Memory::Nx2WarProbeArmResult Memory::Nx2WarProbeArm(std::span<const u64> bank_cpu_pages) {
    return impl->WarArm(bank_cpu_pages);
}

bool Memory::Nx2InflightOpen(u32 fence_id, u32 fence_value, u32 prev_id, u32 prev_value) {
    return impl->InflightOpen(fence_id, fence_value, prev_id, prev_value);
}

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug) {
    impl->MarkRegionDebug(GetInteger(vaddr), size, debug);
}

bool Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size) {
    [[maybe_unused]] bool mapped = true;
    [[maybe_unused]] bool rasterizer = false;

    u8* const ptr = impl->GetPointerImpl(
        GetInteger(vaddr),
        [&] {
            LOG_ERROR(HW_Memory, "Unmapped InvalidateNCE for {} bytes @ {:#x}", size,
                      GetInteger(vaddr));
            mapped = false;
        },
        [&] { rasterizer = true; });
    if (rasterizer) {
        impl->InvalidateGPUMemory(ptr, size);
    }

#ifdef __ANDROID__
    if (!rasterizer && mapped) {
        impl->host_buffer->DeferredMapSeparateHeap(GetInteger(vaddr));
    }
#endif

    return mapped && ptr != nullptr;
}

bool Memory::InvalidateSeparateHeap(void* fault_address) {
#ifdef __ANDROID__
    return impl->host_buffer->DeferredMapSeparateHeap(static_cast<u8*>(fault_address));
#else
    return false;
#endif
}

} // namespace Core::Memory
