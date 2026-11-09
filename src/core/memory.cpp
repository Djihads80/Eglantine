// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_set>
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
#include "core/arm/nce_stats.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/buffer_cache/hot_pages.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2.h"
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
        if (process.IsApplication()) {
            const bool read_traps = ReadTrapsEnabled();
            Tegra::g_nx2_cpu_read_traps.store(read_traps, std::memory_order_relaxed);
            LOG_INFO(HW_Memory, "Read traps on GPU-written pages: {}{}", read_traps ? "on" : "off",
                     Settings::IsNceEnabled() ? " (NCE, EDEN_NX2_NCE_READ_TRAPS)" : "");
        }
    }

    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}",
                   GetInteger(target));
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
            const bool stats = Core::NceStats::Enabled();
            const u64 start = stats ? Core::NceStats::Ticks() : 0;
            host_buffer->Protect(vaddr, size, perm);
            VideoCommon::HotPages::NoteProtect(cached, size);
            if (stats) {
                auto& shared = Core::NceStats::Shared();
                shared.cache_protects.fetch_add(1, std::memory_order_relaxed);
                shared.cache_protect_bytes.fetch_add(size, std::memory_order_relaxed);
                const u64 ticks = Core::NceStats::Ticks() - start;
                shared.cache_protect_ticks.fetch_add(ticks, std::memory_order_relaxed);
                if (system.GetCurrentHostThreadID() < Core::Hardware::NUM_CPU_CORES) {
                    shared.cache_protects_cores.fetch_add(1, std::memory_order_relaxed);
                    shared.cache_protect_ticks_cores.fetch_add(ticks, std::memory_order_relaxed);
                }
            }
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
    /// (EDEN_NX2_READ_TRAPS=0 turns them off). Under NCE the fault handler tells a read from a
    /// write by the fault's ESR and takes a read of an armed page here (TakeNceReadTrap);
    /// EDEN_NX2_NCE_READ_TRAPS=0 turns them off there alone, and every NCE fault on a GPU-cached
    /// page is taken as a write again.
    bool ReadTrapsEnabled() const {
        static const bool disabled = [] {
            const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
            return value != nullptr && value[0] == '0';
        }();
        return !disabled && current_page_table->fastmem_arena &&
               !Settings::values.use_reactive_flushing.GetValue() &&
               (Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce ||
                NceReadTrapsAllowed());
    }

    static bool NceReadTrapsAllowed() {
        static const bool allowed = [] {
            const char* value = std::getenv("EDEN_NX2_NCE_READ_TRAPS");
            return value == nullptr || value[0] != '0';
        }();
        return allowed;
    }

    void RasterizerArmReadTraps(u64 vaddr, u64 size) {
        // Clean cached read areas also need invalidation when reactive flushing is enabled.
        read_trap_generation.fetch_add(1, std::memory_order_release);
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
            if (Settings::IsNceEnabled()) {
                read_trap_pages.insert(page);
            }
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

    bool IsArmedReadTrap(u64 page) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        std::scoped_lock lk{read_trap_mutex};
        return armed_read_traps.contains(page);
    }

    /// Memory::InvalidateNCE, a read fault on a GPU-cached page: when the page is an armed read
    /// trap, the GPU's writes to it are downloaded (OnCPURead, which also marks the range
    /// Preflushable, so later GPU writes to it are copied back at fences) and only then is the
    /// trap disarmed, leaving the page read-only so that a write still faults. Unlike the slow
    /// path (HandleRasterizerDownload) the page stays unreadable until the download is in guest
    /// memory, and one core takes a trap at a time: another core reading the page meanwhile
    /// faults and waits here instead of reading stale data. Returns whether the page was a trap.
    bool TakeNceReadTrap(u64 vaddr) {
        const u64 page = vaddr >> YUZU_PAGEBITS;
        if (!IsArmedReadTrap(page)) {
            return false;
        }
        std::scoped_lock lk{nce_read_trap_mutex};
        if (!IsArmedReadTrap(page)) {
            return true; // Another core took it while this one waited: the data is in.
        }
        const auto start = std::chrono::steady_clock::now();
        const u64 page_address = page << YUZU_PAGEBITS;
        const u8* const p = GetPointerImpl(page_address, [] {}, [] {});
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        if (p != nullptr) {
            gpu_device_memory->ApplyOpOnPointer(p, nce_read_trap_scratch, [&](DAddr address) {
                (void)system.GPU().OnCPURead(address, YUZU_PAGESIZE);
            });
        }
        DisarmNceReadTrap(page);
        NoteNceReadTrap(std::chrono::steady_clock::now() - start);
        return true;
    }

    /// TakeNceReadTrap: the page back to read-only, or read-write when it left the GPU caches
    /// meanwhile (as RasterizerMarkRegionCached leaves an uncached page), so that an uncached
    /// page is never left read-only: no fault on it could lift that.
    void DisarmNceReadTrap(u64 page) {
        std::scoped_lock lk{read_trap_mutex};
        if (armed_read_traps.erase(page) == 0) {
            return; // A slow-path read disarmed it meanwhile.
        }
        armed_read_trap_count.fetch_sub(1, std::memory_order_relaxed);
        const auto is_cached = [&] {
            return std::as_const(current_page_table->entries)[page].Type() ==
                   Common::PageType::RasterizerCachedMemory;
        };
        const bool cached = is_cached();
        host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                             cached ? Common::MemoryPermission::Read
                                    : Common::MemoryPermission::ReadWrite);
        if (cached && !is_cached()) {
            host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                                 Common::MemoryPermission::ReadWrite);
        }
    }

    /// A fault that keeps coming back on a page a read trap was armed on, which is no longer
    /// GPU-cached: a trap's protection change raced the page leaving the caches, and no fault
    /// handling would lift it. Gives the page an uncached page's permissions.
    bool RepairNceReadTrapPage(u64 vaddr) {
        const u64 page = vaddr >> YUZU_PAGEBITS;
        std::scoped_lock lk{read_trap_mutex};
        if (!read_trap_pages.contains(page) ||
            std::as_const(current_page_table->entries)[page].Type() != Common::PageType::Memory) {
            return false;
        }
        host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                             Common::MemoryPermission::ReadWrite);
        return true;
    }

    /// NCE read traps taken, logged every 10 s (or at the first trap after that): how many a
    /// second and what they cost the faulting core. Under nce_read_trap_mutex.
    void NoteNceReadTrap(std::chrono::steady_clock::duration taken) {
        auto& window = nce_read_trap_window;
        const auto now = std::chrono::steady_clock::now();
        if (window.total_traps == 0) {
            window.since = now; // The first one is logged at once.
        }
        ++window.count;
        ++window.total_traps;
        window.taken += taken;
        window.max_taken = (std::max)(window.max_taken, taken);
        const auto elapsed = now - window.since;
        if (window.total_traps != 1 && elapsed < std::chrono::seconds{10}) {
            return;
        }
        using Micros = std::chrono::duration<double, std::micro>;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        LOG_INFO(HW_Memory,
                 "NCE read traps: {} in {:.1f} s ({:.1f}/s), {:.0f} us avg, {:.0f} us max, {} "
                 "since start, {} armed now",
                 window.count, seconds, seconds > 0.0 ? window.count / seconds : 0.0,
                 Micros(window.taken).count() / static_cast<double>(window.count),
                 Micros(window.max_taken).count(), window.total_traps,
                 armed_read_trap_count.load(std::memory_order_relaxed));
        window.since = now;
        window.count = 0;
        window.taken = {};
        window.max_taken = {};
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

            page_table.entries.ZeroRegion(base, end);
        } else {
            u64 mapping_id = block_count.load(std::memory_order_relaxed);
            constexpr u64 exhausted_count =
                u64{Common::PageTable::PageEntryData::INVALID_BLOCK_ID} + 1;
            while (mapping_id < exhausted_count &&
                   !block_count.compare_exchange_weak(mapping_id, mapping_id + 1,
                                                      std::memory_order_relaxed)) {
            }
            const u16 current_block =
                Common::PageTable::PageEntryData::BlockIdForMapping(mapping_id);
            if (mapping_id == Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
                LOG_WARNING(HW_Memory,
                            "Page table block ids are exhausted; validating spans page by page");
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
        if (auto const ptr = GetPointerImpl(addr, [addr, data]() {
            LOG_ERROR(HW_Memory, "Unmapped Write{} @ {:#016x} = {:#016x}", sizeof(T) * 8, addr, u64(data));
        }, [&]() { HandleRasterizerWrite(addr, sizeof(T)); }); ptr) [[likely]]
            std::memcpy(ptr, &data, sizeof(T));
    }

    template <typename T>
    bool WriteExclusive(Common::ProcessAddress vaddr, const T data, const T expected) {
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
                DisarmReadTraps(v_address, size);
                (void)system.GPU().OnCPURead(address, size);
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
            DisarmReadTraps(v_address, size);
            current_area = system.GPU().OnCPURead(address, size);
        });
    }

    void HandleRasterizerWrite(VAddr v_address, size_t size) {
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

    /// Memory::InvalidateNCEAhead: the GPU-cached pages from vaddr, a device-contiguous run at a
    /// time (one cache lock and one protection change per run, not per page).
    size_t InvalidateNCEAhead(u64 vaddr, size_t pages) {
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(), sys_core);
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        auto& gpu = system.GPU();
        size_t done = 0;
        DAddr run_begin = 0;
        u64 run_size = 0;
        size_t run_pages = 0;
        // A run with GPU-written data is left alone, with the pages after it.
        const auto flush = [&] {
            const bool invalidated = run_size == 0 || gpu.TryInvalidateRegion(run_begin, run_size);
            if (invalidated) {
                done += run_pages;
            }
            run_size = 0;
            run_pages = 0;
            return invalidated;
        };
        for (size_t i = 0; i < pages; ++i) {
            bool rasterizer = false;
            u8* const ptr =
                GetPointerImpl(vaddr + i * YUZU_PAGESIZE, [] {}, [&] { rasterizer = true; });
            if (!rasterizer || ptr == nullptr) {
                break;
            }
            // One device address (no aliases), as a write fault on the page would find.
            u32 count = 0;
            DAddr address = 0;
            gpu_device_memory->ApplyOpOnPointer(ptr, scratch_buffers[core], [&](DAddr value) {
                ++count;
                address = value;
            });
            if (count != 1) {
                break;
            }
            if (run_size != 0 && address != run_begin + run_size && !flush()) {
                return done;
            }
            if (run_size == 0) {
                run_begin = address;
            }
            run_size += YUZU_PAGESIZE;
            ++run_pages;
        }
        flush();
        return done;
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
    /// NCE: every page a read trap was armed on (RepairNceReadTrapPage). Under read_trap_mutex.
    std::unordered_set<u64> read_trap_pages;
    /// NCE: held while a core takes a read trap (TakeNceReadTrap), with what only it uses.
    std::mutex nce_read_trap_mutex;
    Common::ScratchBuffer<u32> nce_read_trap_scratch;
    struct NceReadTrapWindow {
        std::chrono::steady_clock::time_point since{};
        u64 count{};
        u64 total_traps{};
        std::chrono::steady_clock::duration taken{};
        std::chrono::steady_clock::duration max_taken{};
    } nce_read_trap_window;
    std::array<GPUDirtyState, Core::Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::array<Common::ScratchBuffer<u32>, Core::Hardware::NUM_CPU_CORES> scratch_buffers{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;
#ifdef __ANDROID__
    std::optional<Common::HeapTracker> heap_tracker;
    Common::HeapTracker* host_buffer{};
#else
    Common::HostMemory* host_buffer{};
#endif
    std::atomic<u64> block_count = 0;
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

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug) {
    impl->MarkRegionDebug(GetInteger(vaddr), size, debug);
}

bool Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size, bool is_read,
                           NceInvalidateResult* result) {
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

    [[maybe_unused]] bool separate_heap_mapped = false;
    bool read_trap = false;
    if (rasterizer && is_read && ptr != nullptr && impl->ReadTrapsEnabled()) {
        // A read: an armed read trap has GPU-written data to download, and a GPU-cached page
        // the heap tracker unmapped only needs mapping back in. Neither invalidates the caches;
        // anything else is taken as a write below, as before read traps.
        read_trap = impl->TakeNceReadTrap(GetInteger(vaddr));
#ifdef __ANDROID__
        separate_heap_mapped = impl->host_buffer->DeferredMapSeparateHeap(GetInteger(vaddr));
#endif
    }
    const bool invalidated = rasterizer && !read_trap && !separate_heap_mapped;
    if (invalidated) {
        impl->InvalidateGPUMemory(ptr, size);
        VideoCommon::HotPages::NoteFault();
    }

#ifdef __ANDROID__
    if (!rasterizer && mapped) {
        separate_heap_mapped = impl->host_buffer->DeferredMapSeparateHeap(GetInteger(vaddr));
    }
#endif
    if (result) {
        *result = {.mapped = mapped && ptr != nullptr,
                   .rasterizer = invalidated,
                   .separate_heap_mapped = separate_heap_mapped,
                   .read_trap = read_trap};
    }

    return mapped && ptr != nullptr;
}

bool Memory::RepairNceReadTrapPage(Common::ProcessAddress vaddr) {
    return impl->RepairNceReadTrapPage(GetInteger(vaddr));
}

bool Memory::IsRasterizerCachedPage(Common::ProcessAddress vaddr) const {
    bool rasterizer = false;
    [[maybe_unused]] const u8* const ptr =
        impl->GetPointerImpl(GetInteger(vaddr), [] {}, [&] { rasterizer = true; });
    return rasterizer;
}

size_t Memory::InvalidateNCEAhead(Common::ProcessAddress vaddr, size_t pages) {
    return impl->InvalidateNCEAhead(GetInteger(vaddr), pages);
}

bool Memory::InvalidateSeparateHeap(void* fault_address) {
#ifdef __ANDROID__
    return impl->host_buffer->DeferredMapSeparateHeap(static_cast<u8*>(fault_address));
#else
    return false;
#endif
}

} // namespace Core::Memory
