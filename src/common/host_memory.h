// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Common {

enum class MemoryPermission : u32 {
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * A low level linear memory buffer, which supports multiple mappings
 * Its purpose is to rebuild a given sparse memory layout, including mirrors.
 */
/// Whether HostMemory::HintCold pages memory out. Its passes stall the GPU threads for as long as
/// they run (Mario Kart World: 60-700 ms every 600 frames, the longer the session the longer).
/// The cold hint mode in effect: the cold_hint_mode setting (off by default), unless
/// EDEN_NX2_COLD_HINT (off, low, always) or the MCP cold_hints tool overrides it.
[[nodiscard]] Settings::ColdHintMode ColdHintModeInEffect();
/// Overrides the setting until the process ends.
void OverrideColdHintMode(Settings::ColdHintMode mode);

/// Whether the system is short of memory: memory pressure stalls over 5% of the last 10 s (Linux
/// PSI, this process's cgroup or the whole system), or less than a tenth of RAM (at least 1 GiB)
/// available. Re-read at most once a second.
[[nodiscard]] bool MemoryIsLow();

enum class ColdHintAction { None, MarkedCold, PagedOut };

class HostMemory {
public:
    explicit HostMemory(size_t backing_size_, size_t virtual_size_);
    ~HostMemory();

    /**
     * Copy constructors. They shall return a copy of the buffer without the mappings.
     * TODO: Implement them with COW if needed.
     */
    HostMemory(const HostMemory& other) = delete;
    HostMemory& operator=(const HostMemory& other) = delete;

    /**
     * Move constructors. They will move the buffer and the mappings to the new object.
     */
    HostMemory(HostMemory&& other) noexcept;
    HostMemory& operator=(HostMemory&& other) noexcept;

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms,
             bool separate_heap);

    void Unmap(size_t virtual_offset, size_t length, bool separate_heap);

    void Protect(size_t virtual_offset, size_t length, MemoryPermission perms);

    void EnableDirectMappedAddress();

    /// Clears guest memory being handed out. Memory still zero since it was created (or since its
    /// last full clear) is skipped: clearing it only made the host give every page real memory.
    void ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value);

    /// Memory written once and read again rarely: its pages leave the working set (backing and,
    /// when given, its view at virtual_offset), so the OS compresses or pages them out first when
    /// it needs RAM. The contents stay; using the pages brings them back.
    ///
    /// What it does follows ColdHintModeInEffect: Always pages the memory out now; WhenRamIsLow
    /// only marks it (MADV_COLD on Linux, so the kernel reclaims it first when it needs RAM) unless
    /// MemoryIsLow, then pages it out. Paging out works through the range in 8 MiB pieces and
    /// pauses between them, so other threads aren't stalled for the whole pass. Call it from a
    /// background thread.
    ColdHintAction HintCold(size_t physical_offset, size_t length,
                            std::optional<size_t> virtual_offset) const;
    /// Pages the kernel hands out (cleared or not).
    void MarkUsed(size_t physical_offset, size_t length);

    /// Pages given back to the kernel: their contents are no longer known to be zero.
    void MarkDirty(size_t physical_offset, size_t length);

    /// A new kernel on this memory: everything a previous one used may hold data.
    void ForgetContents();

    [[nodiscard]] u8* BackingBasePointer() noexcept {
        return backing_base;
    }
    [[nodiscard]] const u8* BackingBasePointer() const noexcept {
        return backing_base;
    }

    [[nodiscard]] u8* VirtualBasePointer() noexcept {
        return virtual_base;
    }
    [[nodiscard]] const u8* VirtualBasePointer() const noexcept {
        return virtual_base;
    }

    [[nodiscard]] size_t VirtualSize() const noexcept {
        return virtual_size;
    }

    bool IsInVirtualRange(void* address) const noexcept {
        return address >= virtual_base && address < virtual_base + virtual_size;
    }

private:
    size_t backing_size{};
    size_t virtual_size{};

#if !(defined(__OPENORBIS__) || defined(__managarm__))
    // Low level handler for the platform dependent memory routines
    class Impl;
    std::unique_ptr<Impl> impl;
#endif
    u8* backing_base{};
    u8* virtual_base{};
    size_t virtual_base_offset{};

    static constexpr size_t GranuleShift = 16; ///< 64 KiB of backing per state byte
    static constexpr u8 GranuleUsed = 1 << 0;   ///< Handed out at least once
    static constexpr u8 GranuleDirty = 1 << 1;  ///< May hold non-zero data
    std::unique_ptr<std::atomic<u8>[]> granules;
    size_t num_granules{};
    // Windows requires it for kernels whom lack proper support for some functions!
    bool fallback_buffer{false};
};

} // namespace Common
