// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <Hypervisor/Hypervisor.h>

#include "common/common_types.h"
#include "common/host_memory.h"
#include "core/hardware_properties.h"

namespace Common {
struct PageTable;
}

namespace Core {

class System;

/// The process-wide Hypervisor.framework VM that application code runs in on Apple Silicon.
///
/// Emulated DRAM is mapped at IPA 0, so a guest physical address is its offset into DeviceMemory.
/// Guest virtual memory is described by 4K-granule stage-1 tables that this class keeps in sync
/// with the emulated kernel's mappings; the host's 16K page size only applies to stage 2.
class HvVm {
public:
    /// Returns the active VM, or nullptr when the hypervisor backend is not in use.
    static HvVm* Get();

    /// Creates the VM (or resets the existing one for a new process). Returns nullptr on failure.
    static HvVm* Create(System& system);

    void Map(u64 vaddr, u64 phys_offset, u64 size, Common::MemoryPermission perms);
    void Unmap(u64 vaddr, u64 size);
    void Protect(u64 vaddr, u64 size, Common::MemoryPermission perms);
    /// Changes read/write access only, keeping each page's execute permission (GPU tracking).
    void ProtectAccess(u64 vaddr, u64 size, Common::MemoryPermission perms);
    /// Stops routing mappings to the VM until the next Create.
    static void Deactivate();

    /// Installs a mapping for vaddr from the emulated page table if it exists there but not in
    /// the stage-1 tables. Returns true if a mapping that allows access was added.
    bool MapFromPageTable(const Common::PageTable& page_table, u64 vaddr);

    /// Returns whether the current stage-1 entry for vaddr allows the access.
    bool Permits(u64 vaddr, bool write);

    /// Makes every vCPU flush its TLB and instruction cache before it next runs guest code.
    void RequestFlush();

    u64 FlushGeneration() const {
        return m_flush_generation.load(std::memory_order_acquire);
    }

    void SetRunning(size_t core, hv_vcpu_t vcpu, bool running);

    static constexpr u64 VectorsVa = 0x1000;
    /// Offset of the "hvc" in each vector slot; the guest's x0 is in TPIDR_EL1 once it is reached.
    static constexpr u64 VectorHvcOffset = 8;
    static constexpr u64 FlushStubVa = VectorsVa + 0x800;
    static constexpr u64 TableBaseIpa = 1ULL << 39;

    u64 RootTableIpa() const {
        return TableBaseIpa + RootTableOffset;
    }

private:
    HvVm() = default;

    bool Initialize(System& system);
    bool MapDram(System& system);
    void Reset();

    u64* EntryFor(u64 vaddr, bool allocate);
    u64* TableAt(u64 table_ipa);
    void SetPage(u64 vaddr, u64 entry);

    static constexpr u64 TableRegionSize = 1ULL << 30;
    static constexpr u64 RootTableOffset = 0x1000;

    u8* m_dram_base{};
    u64 m_dram_size{};
    u8* m_tables{};
    u64 m_next_table_offset{};
    std::mutex m_table_mutex;

    std::atomic<u64> m_flush_generation{1};
    std::mutex m_running_mutex;
    std::array<hv_vcpu_t, Hardware::NUM_CPU_CORES> m_vcpus{};
    std::array<bool, Hardware::NUM_CPU_CORES> m_running{};
};

} // namespace Core
