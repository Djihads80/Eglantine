// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "memory_manager.h"
#include "video_core/guest_memory.h"
#include "video_core/nx2.h"
#include "video_core/nx2_perf.h"
#include "video_core/host1x/host1x.h"
#include "video_core/invalidation_accumulator.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/texture_cache/util.h"

namespace Tegra {
bool MemoryManager::IsShuttingDown() const {
    return system.IsShuttingDown();
}

using Tegra::Memory::GuestMemoryFlags;

std::atomic<size_t> MemoryManager::unique_identifier_generator{};

MemoryManager::MemoryManager(Core::System& system_, MaxwellDeviceMemoryManager& memory_, u64 address_space_bits_, GPUVAddr split_address_, u64 big_page_bits_, u64 page_bits_)
    : system{system_}, memory{memory_}, address_space_bits{address_space_bits_}
    , split_address{split_address_}, big_page_bits{big_page_bits_}
    , entries{}, big_entries{}
    , page_table{address_space_bits, address_space_bits + page_bits - 38, page_bits != big_page_bits ? page_bits : 0}
    , kind_map{PTEKind::INVALID}, unique_identifier{unique_identifier_generator.fetch_add(1, std::memory_order_acq_rel)}
    , accumulator{}
{
    ASSERT(page_bits_ == page_bits);
    address_space_size = 1ULL << address_space_bits;
    big_page_size = 1ULL << big_page_bits;
    big_page_mask = big_page_size - 1ULL;
    const u64 page_table_bits = address_space_bits - page_bits;
    const u64 big_page_table_bits = address_space_bits - big_page_bits;
    const u64 page_table_size = 1ULL << page_table_bits;
    const u64 big_page_table_size = 1ULL << big_page_table_bits;
    page_table_mask = page_table_size - 1;
    big_page_table_mask = big_page_table_size - 1;

    big_page_table_dev.ResizeAndClear(big_page_table_size);
    big_entries.resize(big_page_table_size / 32, 0);
    big_page_continuous.resize(big_page_table_size / continuous_bits, 0);
    entries.resize(page_table_size / 32, 0);
}

MemoryManager::MemoryManager(Core::System& system_, u64 address_space_bits_, GPUVAddr split_address_, u64 big_page_bits_, u64 page_bits_)
    : MemoryManager(system_, system_.Host1x().MemoryManager(), address_space_bits_, split_address_, big_page_bits_, page_bits_)
{}

void MemoryManager::SetNativeSyncpointShim(GPUVAddr address) {
    native_syncpoint_shim.store(address, std::memory_order_release);
}

MemoryManager::~MemoryManager() = default;

MemoryManager::EntryType MemoryManager::GetEntry(size_t position, bool is_big_page) const {
    if (is_big_page) {
        position = position >> big_page_bits;
        const u64 entry_mask = big_entries[position / 32];
        const size_t sub_index = position % 32;
        return static_cast<EntryType>((entry_mask >> (2 * sub_index)) & 0x03ULL);
    } else {
        position = position >> page_bits;
        const u64 entry_mask = entries[position / 32];
        const size_t sub_index = position % 32;
        return static_cast<EntryType>((entry_mask >> (2 * sub_index)) & 0x03ULL);
    }
}

void MemoryManager::SetEntry(size_t position, MemoryManager::EntryType entry, bool is_big_page) {
    if (is_big_page) {
        position = position >> big_page_bits;
        const u64 entry_mask = big_entries[position / 32];
        const size_t sub_index = position % 32;
        big_entries[position / 32] =
            (~(3ULL << sub_index * 2) & entry_mask) | (static_cast<u64>(entry) << sub_index * 2);
    } else {
        position = position >> page_bits;
        const u64 entry_mask = entries[position / 32];
        const size_t sub_index = position % 32;
        entries[position / 32] =
            (~(3ULL << sub_index * 2) & entry_mask) | (static_cast<u64>(entry) << sub_index * 2);
    }
}

PTEKind MemoryManager::GetPageKind(GPUVAddr gpu_addr) const {
    std::unique_lock<std::mutex> lock(guard);
    return kind_map.GetValueAt(gpu_addr);
}

inline bool MemoryManager::IsBigPageContinuous(size_t big_page_index) const {
    const u64 entry_mask = big_page_continuous[big_page_index / continuous_bits];
    const size_t sub_index = big_page_index % continuous_bits;
    return ((entry_mask >> sub_index) & 0x1ULL) != 0;
}

inline void MemoryManager::SetBigPageContinuous(size_t big_page_index, bool value) {
    const u64 continuous_mask = big_page_continuous[big_page_index / continuous_bits];
    const size_t sub_index = big_page_index % continuous_bits;
    big_page_continuous[big_page_index / continuous_bits] =
        (~(1ULL << sub_index) & continuous_mask) | (value ? 1ULL << sub_index : 0);
}

GPUVAddr MemoryManager::PageTableOp(GPUVAddr gpu_addr, [[maybe_unused]] DAddr dev_addr, size_t size, PTEKind kind, MemoryManager::EntryType entry_type) {
    [[maybe_unused]] u64 remaining_size{size};
    if (entry_type == EntryType::Mapped) {
        page_table.ReserveRange(gpu_addr, size);
    }
    // The caches hear about each run of pages whose entry changed in one call, not page by page
    // (a 1.45 GB Switch 2 region made ~370,000 texture cache scans).
    GPUVAddr modified_start{};
    u64 modified_size{};
    const auto flush_modified = [&] {
        if (modified_size != 0) {
            rasterizer->ModifyGPUMemory(unique_identifier, modified_start, modified_size);
            modified_size = 0;
        }
    };
    for (u64 offset{}; offset < size; offset += page_size) {
        const GPUVAddr current_gpu_addr = gpu_addr + offset;
        [[maybe_unused]] const auto current_entry_type = GetEntry(current_gpu_addr, false);
        SetEntry(current_gpu_addr, entry_type, false);
        const bool changed_backing = entry_type == EntryType::Mapped &&
            current_entry_type == EntryType::Mapped &&
            page_table[PageEntryIndex(current_gpu_addr, false)] !=
                static_cast<u32>((dev_addr + offset) >> cpu_page_bits);
        if (current_entry_type != entry_type || changed_backing) {
            if (modified_size == 0) {
                modified_start = current_gpu_addr;
            }
            modified_size += page_size;
        } else {
            flush_modified();
        }
        if (entry_type == EntryType::Mapped) {
            const DAddr current_dev_addr = dev_addr + offset;
            const auto index = PageEntryIndex(current_gpu_addr, false);
            const u32 sub_value = static_cast<u32>(current_dev_addr >> cpu_page_bits);
            page_table[index] = sub_value;
        }
        remaining_size -= page_size;
    }
    flush_modified();
    {
        std::unique_lock<std::mutex> lock(guard);
        kind_map.Map(gpu_addr, gpu_addr + size, kind);
    }
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Bounds)) {
        UpdateContinuousRuns(gpu_addr, size);
    } else {
        map_serial.fetch_add(1, std::memory_order_release);
    }
    return gpu_addr;
}

GPUVAddr MemoryManager::BigPageTableOp(GPUVAddr gpu_addr, [[maybe_unused]] DAddr dev_addr, size_t size, PTEKind kind, MemoryManager::EntryType entry_type) {
    [[maybe_unused]] u64 remaining_size{size};
    GPUVAddr modified_start{};
    u64 modified_size{};
    const auto flush_modified = [&] {
        if (modified_size != 0) {
            rasterizer->ModifyGPUMemory(unique_identifier, modified_start, modified_size);
            modified_size = 0;
        }
    };
    for (u64 offset{}; offset < size; offset += big_page_size) {
        const GPUVAddr current_gpu_addr = gpu_addr + offset;
        [[maybe_unused]] const auto current_entry_type = GetEntry(current_gpu_addr, true);
        SetEntry(current_gpu_addr, entry_type, true);
        const bool changed_backing = entry_type == EntryType::Mapped &&
            current_entry_type == EntryType::Mapped &&
            big_page_table_dev[PageEntryIndex(current_gpu_addr, true)] !=
                static_cast<u32>((dev_addr + offset) >> cpu_page_bits);
        if (current_entry_type != entry_type || changed_backing) {
            if (modified_size == 0) {
                modified_start = current_gpu_addr;
            }
            modified_size += big_page_size;
        } else {
            flush_modified();
        }
        if (entry_type == EntryType::Mapped) {
            const DAddr current_dev_addr = dev_addr + offset;
            const auto index = PageEntryIndex(current_gpu_addr, true);
            const u32 sub_value = static_cast<u32>(current_dev_addr >> cpu_page_bits);
            big_page_table_dev.Set(index, sub_value);
            const bool is_continuous = ([&] {
                uintptr_t base_ptr{
                    reinterpret_cast<uintptr_t>(memory.GetPointer<u8>(current_dev_addr))};
                if (base_ptr == 0) {
                    return false;
                }
                for (DAddr start_cpu = current_dev_addr + page_size;
                     start_cpu < current_dev_addr + big_page_size; start_cpu += page_size) {
                    base_ptr += page_size;
                    auto next_ptr = reinterpret_cast<uintptr_t>(memory.GetPointer<u8>(start_cpu));
                    if (next_ptr == 0 || base_ptr != next_ptr) {
                        return false;
                    }
                }
                return true;
            })();
            SetBigPageContinuous(index, is_continuous);
        }
        remaining_size -= big_page_size;
    }
    flush_modified();
    {
        std::unique_lock<std::mutex> lock(guard);
        kind_map.Map(gpu_addr, gpu_addr + size, kind);
    }
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Bounds)) {
        UpdateContinuousRuns(gpu_addr, size);
    } else {
        map_serial.fetch_add(1, std::memory_order_release);
    }
    return gpu_addr;
}

void MemoryManager::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void MemoryManager::PreserveMapping(GPUVAddr gpu_addr, std::size_t size) {
    if (!UsesNativeTiling() || rasterizer == nullptr || size == 0) {
        return;
    }
    boost::container::small_vector<std::pair<DAddr, std::size_t>, 32> ranges;
    GetSubmappedRangeImpl<false>(gpu_addr, size, ranges);
    for (const auto& [address, bytes] : ranges) {
        rasterizer->FlushRegion(address, bytes, VideoCommon::CacheType::All);
    }
}

GPUVAddr MemoryManager::Map(GPUVAddr gpu_addr, DAddr dev_addr, std::size_t size, PTEKind kind, bool is_big_pages) {
    if (UsesNativeTiling() && rasterizer != nullptr && size != 0 &&
        (!IsContinuousRange(gpu_addr, size) || GpuToCpuAddress(gpu_addr) != dev_addr)) {
        PreserveMapping(gpu_addr, size);
        // Another virtual view can own the newest image contents at this device address.
        // Publish it while its translation is valid, then make sampled aliases reload it.
        rasterizer->FlushRegion(dev_addr, size, VideoCommon::CacheType::All);
        rasterizer->InvalidateRegion(dev_addr, size, VideoCommon::CacheType::TextureCache);
    }
    if (is_big_pages)
        return BigPageTableOp(gpu_addr, dev_addr, size, kind, EntryType::Mapped);
    return PageTableOp(gpu_addr, dev_addr, size, kind, EntryType::Mapped);
}

GPUVAddr MemoryManager::MapSparse(GPUVAddr gpu_addr, std::size_t size, bool is_big_pages) {
    PreserveMapping(gpu_addr, size);
    if (is_big_pages)
        return BigPageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Reserved);
    return PageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Reserved);
}

void MemoryManager::Unmap(GPUVAddr gpu_addr, std::size_t size) {
    if (size == 0) {
        return;
    }
    PreserveMapping(gpu_addr, size);
    GetSubmappedRangeImpl<false>(gpu_addr, size, page_stash);

    for (const auto& [map_addr, map_size] : page_stash) {
        rasterizer->UnmapMemory(map_addr, map_size);
    }
    page_stash.clear();

    BigPageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Free);
    PageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Free);
}

std::optional<DAddr> MemoryManager::GpuToCpuAddress(GPUVAddr gpu_addr) const {
    if (!IsWithinGPUAddressRange(gpu_addr)) [[unlikely]] {
        return std::nullopt;
    }
    DAddr dev_addr;
    if (GetEntry(gpu_addr, true) != EntryType::Mapped) [[unlikely]] {
        if (GetEntry(gpu_addr, false) != EntryType::Mapped)
            return std::nullopt;
        dev_addr = (DAddr(page_table[PageEntryIndex(gpu_addr, false)]) << cpu_page_bits) +
                   (gpu_addr & page_mask);
    } else {
        dev_addr = (DAddr(big_page_table_dev[PageEntryIndex(gpu_addr, true)]) << cpu_page_bits) +
                   (gpu_addr & big_page_mask);
    }
    if ((dev_addr >> MaxwellDeviceMemoryManager::AS_BITS) != 0) {
        LOG_ERROR(HW_GPU, "GPU address {:#x} maps outside the device space, to {:#x}", gpu_addr,
                  dev_addr);
        return std::nullopt;
    }
    return dev_addr;
}

std::optional<DAddr> MemoryManager::GpuToCpuAddress(GPUVAddr addr, std::size_t size) const {
    size_t page_index{addr >> page_bits};
    const size_t page_last{(addr + size + page_size - 1) >> page_bits};
    while (page_index < page_last) {
        const auto page_addr{GpuToCpuAddress(page_index << page_bits)};
        if (page_addr) {
            return page_addr;
        }
        ++page_index;
    }
    return std::nullopt;
}

template <typename T>
T MemoryManager::Read(GPUVAddr addr) const {
    if constexpr (sizeof(T) == sizeof(u32)) {
        const auto shim = native_syncpoint_shim.load(std::memory_order_acquire);
        if (shim && addr >= shim && addr - shim < NumShimSyncpoints * sizeof(u32) &&
            Common::IsAligned(addr - shim, sizeof(u32))) {
            return static_cast<T>(system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(
                static_cast<u32>((addr - shim) / sizeof(u32))));
        }
    }

    if (auto page_pointer{GetPointer(addr)}; page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        T value;
        std::memcpy(&value, page_pointer, sizeof(T));
        return value;
    }

    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 16) {
        LOG_ERROR(HW_GPU, "Read{} of unmapped GPU address {:#x}", sizeof(T) * 8, addr);
    }

    return {};
}

u32 MemoryManager::ReadU32(GPUVAddr addr, ReadPageCache& cache) const {
    const auto shim = native_syncpoint_shim.load(std::memory_order_acquire);
    if (shim && addr >= shim && addr - shim < NumShimSyncpoints * sizeof(u32)) {
        return Read<u32>(addr);
    }
    const GPUVAddr offset = addr & (page_size - 1);
    if (offset > page_size - sizeof(u32)) {
        return Read<u32>(addr); // Crosses into the next page
    }
    const GPUVAddr page = addr - offset;
    if (page != cache.page) {
        cache.page = page;
        cache.pointer = GetPointer(page);
    }
    if (cache.pointer == nullptr) {
        return Read<u32>(addr); // Unmapped or sparse: Read logs and handles it
    }
    u32 value;
    std::memcpy(&value, cache.pointer + offset, sizeof(value));
    return value;
}

template <typename T>
void MemoryManager::Write(GPUVAddr addr, T data) {
    if (auto page_pointer{GetPointer(addr)}; page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        std::memcpy(page_pointer, &data, sizeof(T));
        return;
    }
    // A Switch 2 sparse block nobody wrote yet gets its memory now.
    Tegra::Nx2BackSparseWrite(addr, sizeof(T), &data);
    if (auto page_pointer{GetPointer(addr)}; page_pointer) {
        std::memcpy(page_pointer, &data, sizeof(T));
        return;
    }

    static std::atomic<u32> reported{};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_ERROR(HW_GPU, "Write{} of unmapped GPU address {:#x}", sizeof(T) * 8, addr);
    }
}

template u8 MemoryManager::Read<u8>(GPUVAddr addr) const;
template u16 MemoryManager::Read<u16>(GPUVAddr addr) const;
template u32 MemoryManager::Read<u32>(GPUVAddr addr) const;
template u64 MemoryManager::Read<u64>(GPUVAddr addr) const;
template void MemoryManager::Write<u8>(GPUVAddr addr, u8 data);
template void MemoryManager::Write<u16>(GPUVAddr addr, u16 data);
template void MemoryManager::Write<u32>(GPUVAddr addr, u32 data);
template void MemoryManager::Write<u64>(GPUVAddr addr, u64 data);

u8* MemoryManager::GetPointer(GPUVAddr gpu_addr) {
    const auto address{GpuToCpuAddress(gpu_addr)};
    if (!address) {
        return {};
    }

    return memory.GetPointer<u8>(*address);
}

const u8* MemoryManager::GetPointer(GPUVAddr gpu_addr) const {
    const auto address{GpuToCpuAddress(gpu_addr)};
    if (!address) {
        return {};
    }

    return memory.GetPointer<u8>(*address);
}

#if defined(_MSC_VER) && !defined(__clang__) // no need for gcc / clang but msvc's compiler is more conservative with inlining.
#pragma inline_recursion(on)
#endif

template <typename FuncMapped, typename FuncReserved, typename FuncUnmapped>
inline void MemoryManager::MemoryOperation(GPUVAddr gpu_src_addr, std::size_t size, bool is_big_page, FuncMapped&& func_mapped, FuncReserved&& func_reserved, FuncUnmapped&& func_unmapped) const {
    if (!IsWithinGPUAddressRange(gpu_src_addr) || size > address_space_size - gpu_src_addr) {
        return;
    }
    using FuncMappedReturn =
        typename std::invoke_result<FuncMapped, std::size_t, std::size_t, std::size_t>::type;
    using FuncReservedReturn =
        typename std::invoke_result<FuncReserved, std::size_t, std::size_t, std::size_t>::type;
    using FuncUnmappedReturn =
        typename std::invoke_result<FuncUnmapped, std::size_t, std::size_t, std::size_t>::type;
    static constexpr bool BOOL_BREAK_MAPPED = std::is_same_v<FuncMappedReturn, bool>;
    static constexpr bool BOOL_BREAK_RESERVED = std::is_same_v<FuncReservedReturn, bool>;
    static constexpr bool BOOL_BREAK_UNMAPPED = std::is_same_v<FuncUnmappedReturn, bool>;
    u64 used_page_size;
    u64 used_page_mask;
    u64 used_page_bits;
    if (is_big_page) {
        used_page_size = big_page_size;
        used_page_mask = big_page_mask;
        used_page_bits = big_page_bits;
    } else {
        used_page_size = page_size;
        used_page_mask = page_mask;
        used_page_bits = page_bits;
    }
    std::size_t remaining_size{size};
    std::size_t page_index{gpu_src_addr >> used_page_bits};
    std::size_t page_offset{gpu_src_addr & used_page_mask};
    GPUVAddr current_address = gpu_src_addr;

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            (std::min)(static_cast<std::size_t>(used_page_size) - page_offset, remaining_size)};
        auto entry = GetEntry(current_address, is_big_page);
        if (entry == EntryType::Mapped) [[likely]] {
            if constexpr (BOOL_BREAK_MAPPED) {
                if (func_mapped(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_mapped(page_index, page_offset, copy_amount);
            }

        } else if (entry == EntryType::Reserved) {
            if constexpr (BOOL_BREAK_RESERVED) {
                if (func_reserved(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_reserved(page_index, page_offset, copy_amount);
            }

        } else [[unlikely]] {
            if constexpr (BOOL_BREAK_UNMAPPED) {
                if (func_unmapped(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_unmapped(page_index, page_offset, copy_amount);
            }
        }
        page_index++;
        page_offset = 0;
        remaining_size -= copy_amount;
        current_address += copy_amount;
    }
}

void MemoryManager::ReadBlockImpl(GPUVAddr gpu_src_addr, void* dest_buffer, std::size_t size, [[maybe_unused]] VideoCommon::CacheType which, bool unsafe) const {
    if (!IsWithinGPUAddressRange(gpu_src_addr) || size > address_space_size - gpu_src_addr) {
        std::memset(dest_buffer, 0, size);
        return;
    }
    auto set_to_zero = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset, std::size_t copy_amount) {
        std::memset(dest_buffer, 0, copy_amount);
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
        }
        // A device page without memory (a sparse block nobody wrote yet) reads zero.
        if (const u8* physical = memory.GetPointer<u8>(dev_addr_base)) [[likely]] {
            std::memcpy(dest_buffer, physical, copy_amount);
        } else {
            std::memset(dest_buffer, 0, copy_amount);
        }
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
        }
        // A continuous big page may still have no host memory (a Switch 2 region backed as it is
        // written, while its backing is pending): the per-page copy handles that.
        u8* const physical = IsBigPageContinuous(page_index) ? memory.GetPointer<u8>(dev_addr_base)
                                                             : nullptr;
        if (physical != nullptr) [[likely]] {
            std::memcpy(dest_buffer, physical, copy_amount);
        } else {
            memory.ReadBlockUnsafe(dev_addr_base, dest_buffer, copy_amount);
        }
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto read_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, set_to_zero, set_to_zero);
    };
    MemoryOperation(gpu_src_addr, size, true, mapped_big, set_to_zero, read_short_pages);
}

void MemoryManager::ReadBlock(GPUVAddr gpu_src_addr, void* dest_buffer, std::size_t size, VideoCommon::CacheType which) const {
    ReadBlockImpl(gpu_src_addr, dest_buffer, size, which, false);
}

void MemoryManager::ReadBlockUnsafe(GPUVAddr gpu_src_addr, void* dest_buffer, const std::size_t size) const {
    // Small reads within one 4 KiB page (texture and sampler descriptors, several per draw): one
    // translation and a copy, as Read<T> does, instead of the page walk (~3% of the GPU thread
    // in title 1). Anything else, unmapped or sparse pages included, takes the full path.
    constexpr std::size_t SmallPage = 0x1000;
    if (size <= SmallPage && (gpu_src_addr & (SmallPage - 1)) + size <= SmallPage) {
        if (const u8* const pointer = GetPointer(gpu_src_addr)) {
            std::memcpy(dest_buffer, pointer, size);
            return;
        }
    }
    ReadBlockImpl(gpu_src_addr, dest_buffer, size, VideoCommon::CacheType::None, true);
}

void MemoryManager::ReadWordsUnsafe(GPUVAddr gpu_src_addr, u32* dest, std::size_t count) const {
    const std::size_t size = count * sizeof(u32);
    const auto shim = native_syncpoint_shim.load(std::memory_order_acquire);
    if (shim && gpu_src_addr < shim + NumShimSyncpoints * sizeof(u32) && gpu_src_addr + size > shim) {
        for (std::size_t i = 0; i < count; ++i) {
            dest[i] = Read<u32>(gpu_src_addr + i * sizeof(u32));
        }
        return;
    }
    ReadBlockUnsafe(gpu_src_addr, dest, size);
}

void MemoryManager::WriteBlockImpl(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size, [[maybe_unused]] VideoCommon::CacheType which, bool unsafe) {
    // Give Switch 2 sparse pages the write lands on their memory first (otherwise it is dropped).
    Tegra::Nx2BackSparseWrite(gpu_dest_addr, size, src_buffer);
    auto just_advance = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset, std::size_t copy_amount) {
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->PrepareRegionWrite(dev_addr_base, copy_amount, which);
        }
        if (u8* physical = memory.GetPointer<u8>(dev_addr_base)) [[likely]] {
            std::memcpy(physical, src_buffer, copy_amount);
        }
        // After the copy: the buffer cache may upload the written bytes from guest memory.
        if (!unsafe) {
            rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
        }
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->PrepareRegionWrite(dev_addr_base, copy_amount, which);
        }
        u8* const physical = IsBigPageContinuous(page_index) ? memory.GetPointer<u8>(dev_addr_base)
                                                             : nullptr;
        if (physical != nullptr) [[likely]] {
            std::memcpy(physical, src_buffer, copy_amount);
        } else {
            memory.WriteBlockUnsafe(dev_addr_base, src_buffer, copy_amount);
        }
        if (!unsafe) {
            rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
        }
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto write_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, just_advance, just_advance);
    };
    MemoryOperation(gpu_dest_addr, size, true, mapped_big, just_advance, write_short_pages);
}

void MemoryManager::WriteBlock(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size, VideoCommon::CacheType which) {
    WriteBlockImpl(gpu_dest_addr, src_buffer, size, which, false);
}

void MemoryManager::WriteBlockUnsafe(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size) {
    WriteBlockImpl(gpu_dest_addr, src_buffer, size, VideoCommon::CacheType::None, true);
}

void MemoryManager::NoteDeferredCodeWrite(GPUVAddr gpu_addr, std::size_t size) const {
    if (size == 0 || !Tegra::g_nx2_has_proven_code.load(std::memory_order_acquire)) {
        return;
    }
    constexpr GPUVAddr code_page = GPUVAddr{1} << Tegra::Nx2WrittenPageBits;
    const GPUVAddr end = gpu_addr + size;
    for (GPUVAddr begin = gpu_addr; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(code_page - 1)) + code_page, end);
        if (const auto device = GpuToCpuAddress(begin)) {
            Tegra::Nx2NoteDeferredCodeWrite(*device, page_end - begin);
        }
        begin = page_end;
    }
}

void MemoryManager::WriteBlockCached(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size) {
    WriteBlockImpl(gpu_dest_addr, src_buffer, size, VideoCommon::CacheType::None, true);
    NoteDeferredCodeWrite(gpu_dest_addr, size);
    accumulator.Add(gpu_dest_addr, size);
}

void MemoryManager::WriteWordsCached(GPUVAddr gpu_dest_addr, const u32* words, std::size_t count) {
    const std::size_t size = count * sizeof(u32);
    // Words on Switch 2 sparse memory go one at a time, as Nx2BackSparseWrite saw them.
    if (gpu_dest_addr + size > Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) &&
        gpu_dest_addr < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed)) {
        for (std::size_t i = 0; i < count; ++i) {
            WriteBlockCached(gpu_dest_addr + i * sizeof(u32), words + i, sizeof(u32));
        }
        return;
    }
    WriteBlockImpl(gpu_dest_addr, words, size, VideoCommon::CacheType::None, true);
    NoteDeferredCodeWrite(gpu_dest_addr, size);
    // The accumulator merges word by word differently from one range: keep its ranges.
    for (std::size_t i = 0; i < count; ++i) {
        accumulator.Add(gpu_dest_addr + i * sizeof(u32), sizeof(u32));
    }
}

void MemoryManager::FlushQueryResults(GPUVAddr gpu_addr, size_t size) const {
    constexpr GPUVAddr flush_page = 0x1000;
    const GPUVAddr end = gpu_addr + size;
    for (GPUVAddr begin = gpu_addr; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(flush_page - 1)) + flush_page, end);
        if (const auto device = GpuToCpuAddress(begin)) {
            rasterizer->FlushQueryResults(*device, page_end - begin);
        }
        begin = page_end;
    }
}

void MemoryManager::FlushRegion(GPUVAddr gpu_addr, size_t size,
                                VideoCommon::CacheType which) const {
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) {};

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
    };
    auto flush_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, flush_short_pages);
}

bool MemoryManager::IsMemoryDirty(GPUVAddr gpu_addr, size_t size,
                                  VideoCommon::CacheType which) const {
    bool result = false;
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) { return false; };

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        result |= rasterizer->MustFlushRegion(dev_addr_base, copy_amount, which);
        return result;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        result |= rasterizer->MustFlushRegion(dev_addr_base, copy_amount, which);
        return result;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
        return result;
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, check_short_pages);
    return result;
}

size_t MemoryManager::MaxContinuousRange(GPUVAddr gpu_addr, size_t size) const {
    // Native storage buffers ask this for up to 8 MiB on each bind, ~90,000 times a second in
    // Ocarina of Time: answer from the runs kept at map time instead of walking pages.
    if (!VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Bounds)) {
        return MaxContinuousRangeUncached(gpu_addr, size);
    }
    size_t result = 0;
    if (IsWithinGPUAddressRange(gpu_addr) && size <= address_space_size - gpu_addr) {
        const auto& runs = CurrentSnapshot().runs;
        struct RunHint {
            const std::vector<MappingSnapshot::Run>* runs{};
            size_t index{};
        };
        thread_local RunHint hint{};
        if (hint.runs == &runs && hint.index < runs.size() &&
            runs[hint.index].begin <= gpu_addr && gpu_addr < runs[hint.index].end) [[likely]] {
            result = (std::min<size_t>)(size, runs[hint.index].end - gpu_addr);
        } else {
            const auto it = std::upper_bound(
                runs.begin(), runs.end(), gpu_addr,
                [](GPUVAddr address, const MappingSnapshot::Run& run) {
                    return address < run.begin;
                });
            if (it != runs.begin() && gpu_addr < std::prev(it)->end) {
                const auto run_it = std::prev(it);
                hint = {&runs, static_cast<size_t>(std::distance(runs.begin(), run_it))};
                result = (std::min<size_t>)(size, run_it->end - gpu_addr);
            }
        }
    }
    if (VideoCommon::Nx2Perf::Verify()) {
        const size_t expected = MaxContinuousRangeUncached(gpu_addr, size);
        static std::atomic<u32> logged{};
        if (expected != result && logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf mapping runs: MaxContinuousRange({:#x}, {:#x}) = {:#x}, walk {:#x}",
                      gpu_addr, size, result, expected);
        }
        return expected;
    }
    return result;
}

void MemoryManager::UpdateContinuousRuns(GPUVAddr gpu_addr, size_t size) {
    // Every entry the operation set lies in these big pages; the walk reads nothing else there.
    const GPUVAddr begin = gpu_addr & ~big_page_mask;
    const GPUVAddr end =
        (std::min)(Common::AlignUp(gpu_addr + size, big_page_size), address_space_size);
    if (size == 0 || begin >= end) {
        map_serial.fetch_add(1, std::memory_order_release);
        return;
    }
    std::vector<std::pair<GPUVAddr, ContinuousRun>> rebuilt;
    const auto add = [&](GPUVAddr address, u64 length, DAddr device_addr) {
        if (!rebuilt.empty()) {
            auto& [run_begin, run] = rebuilt.back();
            if (run.end == address && run.device_addr + (run.end - run_begin) == device_addr) {
                run.end += length;
                return;
            }
        }
        rebuilt.push_back({address, {address + length, device_addr}});
    };
    std::scoped_lock lock{runs_guard};
    // Runs that overlap or touch [begin, end) are cut at its edges and merged again.
    auto it = continuous_runs.lower_bound(begin);
    if (it != continuous_runs.begin() && std::prev(it)->second.end >= begin) {
        --it;
    }
    if (it != continuous_runs.end() && it->first < begin) {
        add(it->first, begin - it->first, it->second.device_addr);
    }
    std::optional<std::pair<GPUVAddr, ContinuousRun>> tail;
    while (it != continuous_runs.end() && it->first <= end) {
        if (it->second.end > end) {
            tail = {end, {it->second.end, it->second.device_addr + (end - it->first)}};
        }
        it = continuous_runs.erase(it);
    }
    for (GPUVAddr page = begin; page < end; page += big_page_size) {
        const EntryType big_entry = GetEntry(page, true);
        if (big_entry == EntryType::Mapped) {
            add(page, big_page_size,
                DAddr(big_page_table_dev[PageEntryIndex(page, true)]) << cpu_page_bits);
        } else if (big_entry == EntryType::Free) {
            for (GPUVAddr small = page; small < page + big_page_size; small += page_size) {
                if (GetEntry(small, false) == EntryType::Mapped) {
                    add(small, page_size,
                        DAddr(page_table[PageEntryIndex(small, false)]) << cpu_page_bits);
                }
            }
        }
    }
    if (tail) {
        add(tail->first, tail->second.end - tail->first, tail->second.device_addr);
    }
    for (const auto& run : rebuilt) {
        continuous_runs.insert(continuous_runs.end(), run);
    }
    map_serial.fetch_add(1, std::memory_order_release);
}

const MemoryManager::MappingSnapshot& MemoryManager::CurrentSnapshot() const {
    struct View {
        size_t owner;
        u64 serial;
        std::shared_ptr<const MappingSnapshot> snapshot;
    };
    thread_local std::array<View, 4> views{};
    View& view = views[unique_identifier % views.size()];
    u64 serial = map_serial.load(std::memory_order_acquire);
    if (view.owner == unique_identifier + 1 && view.serial == serial) {
        return *view.snapshot;
    }
    std::scoped_lock lock{runs_guard};
    serial = map_serial.load(std::memory_order_acquire);
    if (!snapshot || snapshot_serial != serial) {
        auto next = std::make_shared<MappingSnapshot>();
        next->runs.reserve(continuous_runs.size());
        for (const auto& [begin, run] : continuous_runs) {
            next->runs.push_back({begin, run.end});
        }
        {
            std::unique_lock<std::mutex> kind_lock(guard);
            kind_map.ForEachRange(
                [&](s64 start, PTEKind kind) { next->kinds.emplace_back(start, kind); });
        }
        snapshot = std::move(next);
        snapshot_serial = serial;
    }
    view = {unique_identifier + 1, serial, snapshot};
    return *view.snapshot;
}

size_t MemoryManager::MaxContinuousRangeUncached(GPUVAddr gpu_addr, size_t size) const {
    std::optional<DAddr> old_page_addr{};
    size_t range_so_far = 0;
    bool result{false};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    std::size_t copy_amount) {
        result = true;
        return true;
    };
    auto short_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = true;
            return true;
        }
        range_so_far += copy_amount;
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto big_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            return true;
        }
        range_so_far += copy_amount;
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, short_check, fail, fail);
        return result;
    };
    MemoryOperation(gpu_addr, size, true, big_check, fail, check_short_pages);
    return range_so_far;
}

size_t MemoryManager::GetMemoryLayoutSize(GPUVAddr gpu_addr,
                                          [[maybe_unused]] size_t max_size) const {
    if (!VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Bounds)) {
        return GetMemoryLayoutSizeUncached(gpu_addr);
    }
    // RangeMap::GetContinuousSizeFrom on the snapshot's copy of the kind map, without its lock.
    size_t result = 0;
    const s64 address = static_cast<s64>(gpu_addr);
    const auto& kinds = CurrentSnapshot().kinds;
    struct KindHint {
        const std::vector<std::pair<s64, PTEKind>>* kinds{};
        size_t index{};
    };
    thread_local KindHint hint{};
    if (address >= 0 && hint.kinds == &kinds && hint.index < kinds.size() &&
        kinds[hint.index].first <= address &&
        (hint.index + 1 == kinds.size() || address < kinds[hint.index + 1].first)) [[likely]] {
        if (kinds[hint.index].second != PTEKind::INVALID) {
            result = hint.index + 1 == kinds.size()
                         ? static_cast<size_t>((std::numeric_limits<s64>::max)() - address)
                         : static_cast<size_t>(kinds[hint.index + 1].first - address);
        }
    } else {
        auto it = std::upper_bound(kinds.begin(), kinds.end(), address,
                                   [](s64 key, const auto& range) { return key < range.first; });
        if (address >= 0 && it != kinds.begin()) {
            const auto kind_it = std::prev(it);
            hint = {&kinds, static_cast<size_t>(std::distance(kinds.begin(), kind_it))};
            if (kind_it->second != PTEKind::INVALID) {
                result = it == kinds.end()
                             ? static_cast<size_t>((std::numeric_limits<s64>::max)() - address)
                             : static_cast<size_t>(it->first - address);
            }
        }
    }
    if (VideoCommon::Nx2Perf::Verify()) {
        const size_t expected = GetMemoryLayoutSizeUncached(gpu_addr);
        static std::atomic<u32> logged{};
        if (expected != result && logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf mapping runs: GetMemoryLayoutSize({:#x}) = {:#x}, kind map {:#x}",
                      gpu_addr, result, expected);
        }
        return expected;
    }
    return result;
}

size_t MemoryManager::GetMemoryLayoutSizeUncached(GPUVAddr gpu_addr) const {
    std::unique_lock<std::mutex> lock(guard);
    return kind_map.GetContinuousSizeFrom(gpu_addr);
}

void MemoryManager::InvalidateRegion(GPUVAddr gpu_addr, size_t size,
                                     VideoCommon::CacheType which) const {
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) {};

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
    };
    auto invalidate_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, invalidate_short_pages);
}

void MemoryManager::CopyBlock(GPUVAddr gpu_dest_addr, GPUVAddr gpu_src_addr, std::size_t size,
                              VideoCommon::CacheType which) {
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadWrite> data(
        *this, gpu_src_addr, size);
    data.SetAddressAndSize(gpu_dest_addr, size);
    FlushRegion(gpu_dest_addr, size, which);
}

bool MemoryManager::IsGranularRange(GPUVAddr gpu_addr, std::size_t size) const {
    if (!IsWithinGPUAddressRange(gpu_addr) || size > address_space_size - gpu_addr) {
        return false;
    }
    if (GetEntry(gpu_addr, true) == EntryType::Mapped) [[likely]] {
        size_t page_index = gpu_addr >> big_page_bits;
        if (IsBigPageContinuous(page_index)) [[likely]] {
            const std::size_t page{(page_index & big_page_mask) + size};
            return page <= big_page_size;
        }
        const std::size_t page{(gpu_addr & Core::DEVICE_PAGEMASK) + size};
        return page <= Core::DEVICE_PAGESIZE;
    }
    if (GetEntry(gpu_addr, false) != EntryType::Mapped) {
        return false;
    }
    const std::size_t page{(gpu_addr & Core::DEVICE_PAGEMASK) + size};
    return page <= Core::DEVICE_PAGESIZE;
}

bool MemoryManager::IsContinuousRange(GPUVAddr gpu_addr, std::size_t size) const {
    if (!IsWithinGPUAddressRange(gpu_addr) || size > address_space_size - gpu_addr) {
        return false;
    }
    std::optional<DAddr> old_page_addr{};
    bool result{true};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    std::size_t copy_amount) {
        result = false;
        return true;
    };
    auto short_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = false;
            return true;
        }
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto big_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = false;
            return true;
        }
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, short_check, fail, fail);
        return !result;
    };
    MemoryOperation(gpu_addr, size, true, big_check, fail, check_short_pages);
    return result;
}

bool MemoryManager::IsFullyMappedRange(GPUVAddr gpu_addr, std::size_t size) const {
    if (!IsWithinGPUAddressRange(gpu_addr) || size > address_space_size - gpu_addr) {
        return false;
    }
    bool result{true};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    [[maybe_unused]] std::size_t copy_amount) {
        result = false;
        return true;
    };
    auto pass = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    [[maybe_unused]] std::size_t copy_amount) { return false; };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, pass, pass, fail);
        return !result;
    };
    MemoryOperation(gpu_addr, size, true, pass, fail, check_short_pages);
    return result;
}

boost::container::small_vector<std::pair<GPUVAddr, std::size_t>, 32>
MemoryManager::GetSubmappedRange(GPUVAddr gpu_addr, std::size_t size) const {
    boost::container::small_vector<std::pair<GPUVAddr, std::size_t>, 32> result{};
    GetSubmappedRangeImpl<true>(gpu_addr, size, result);
    return result;
}

template <bool is_gpu_address>
void MemoryManager::GetSubmappedRangeImpl(GPUVAddr gpu_addr, std::size_t size, boost::container::small_vector<std::pair<std::conditional_t<is_gpu_address, GPUVAddr, DAddr>, std::size_t>, 32>& result)
    const {
    std::optional<std::pair<std::conditional_t<is_gpu_address, GPUVAddr, DAddr>, std::size_t>> last_segment{};
    std::optional<DAddr> old_page_addr{};
    const auto split = [&last_segment, &result]([[maybe_unused]] std::size_t page_index,
                                                [[maybe_unused]] std::size_t offset,
                                                [[maybe_unused]] std::size_t copy_amount) {
        if (last_segment) {
            result.push_back(*last_segment);
            last_segment = std::nullopt;
        }
    };
    const auto extend_size_big = [this, &split, &old_page_addr,
                                  &last_segment](std::size_t page_index, std::size_t offset,
                                                 std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr) {
            if (*old_page_addr != dev_addr_base) {
                split(0, 0, 0);
            }
        }
        old_page_addr = {dev_addr_base + copy_amount};
        if (!last_segment) {
            if constexpr (is_gpu_address) {
                const GPUVAddr new_base_addr = (page_index << big_page_bits) + offset;
                last_segment = {new_base_addr, copy_amount};
            } else {
                last_segment = {dev_addr_base, copy_amount};
            }
        } else {
            last_segment->second += copy_amount;
        }
    };
    const auto extend_size_short = [this, &split, &old_page_addr,
                                    &last_segment](std::size_t page_index, std::size_t offset,
                                                   std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr) {
            if (*old_page_addr != dev_addr_base) {
                split(0, 0, 0);
            }
        }
        old_page_addr = {dev_addr_base + copy_amount};
        if (!last_segment) {
            if constexpr (is_gpu_address) {
                const GPUVAddr new_base_addr = (page_index << page_bits) + offset;
                last_segment = {new_base_addr, copy_amount};
            } else {
                last_segment = {dev_addr_base, copy_amount};
            }
        } else {
            last_segment->second += copy_amount;
        }
    };
    auto do_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, extend_size_short, split, split);
    };
    MemoryOperation(gpu_addr, size, true, extend_size_big, split, do_short_pages);
    split(0, 0, 0);
}

void MemoryManager::FlushCaching() {
    // Flush from the invalidate accumulator
    if (accumulator.InvalidateAll([this](GPUVAddr addr, size_t size) {
        GetSubmappedRangeImpl<false>(addr, size, page_stash2);
    })) {
        rasterizer->InnerInvalidation(page_stash2);
        page_stash2.clear();
    }
}

const u8* MemoryManager::GetSpan(const GPUVAddr src_addr, const std::size_t size) const {
    if (!IsContinuousRange(src_addr, size)) {
        return nullptr;
    }
    auto dev_addr = GpuToCpuAddress(src_addr);
    if (dev_addr) {
        return memory.GetSpan(*dev_addr, size);
    }
    return nullptr;
}

u8* MemoryManager::GetSpan(const GPUVAddr src_addr, const std::size_t size) {
    if (!IsContinuousRange(src_addr, size)) {
        return nullptr;
    }
    auto dev_addr = GpuToCpuAddress(src_addr);
    if (dev_addr) {
        return memory.GetSpan(*dev_addr, size);
    }
    return nullptr;
}

} // namespace Tegra
