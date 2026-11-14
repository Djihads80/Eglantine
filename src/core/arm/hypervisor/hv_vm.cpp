// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>

#include <sys/mman.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/settings.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_system_control.h"

namespace Core {

namespace {

constexpr u64 HostPageSize = 16 * 1024;

constexpr u64 DescValid = 1ULL << 0;
constexpr u64 DescTableOrPage = 1ULL << 1;
constexpr u64 AttrIndexNormal = 0ULL << 2;
constexpr u64 ApEl0ReadWrite = 1ULL << 6;
constexpr u64 ApEl1ReadOnly = 2ULL << 6;
constexpr u64 ApEl0ReadOnly = 3ULL << 6;
constexpr u64 ShareInner = 3ULL << 8;
constexpr u64 AccessFlag = 1ULL << 10;
constexpr u64 PrivilegedExecuteNever = 1ULL << 53;
constexpr u64 UserExecuteNever = 1ULL << 54;
constexpr u64 AddressMask = 0x000000FFFFFFF000ULL;
constexpr u64 PermissionMask = ApEl0ReadOnly | UserExecuteNever | DescValid;

// Every exception vector traps to the host with "hvc #0". Hypervisor.framework services hvc calls
// whose x0 looks like an SMCCC function ID itself (guest pointers such as 0x84xxxxxx do), so the
// guest's x0 is parked in TPIDR_EL1 and cleared first. The flush stub at +0x800 invalidates the
// TLB and instruction cache for the whole VM and returns with "hvc #1"; the host preserves x0.
constexpr std::array<u32, 3> VectorSlot{
    0xD518D080, // msr tpidr_el1, x0
    0xD2800000, // mov x0, #0
    0xD4000002, // hvc #0
};
constexpr std::array<u32, 7> FlushStub{
    0xD2800000, // mov x0, #0
    0xD508831F, // tlbi vmalle1is
    0xD508711F, // ic ialluis
    0xD5033B9F, // dsb ish
    0xD5033FDF, // isb
    0xD4000022, // hvc #1
    0xD4000022,
};

u64 PermissionBits(Common::MemoryPermission perms) {
    if (perms == Common::MemoryPermission{}) {
        return 0;
    }
    u64 bits = DescValid | PrivilegedExecuteNever;
    bits |= True(perms & Common::MemoryPermission::Write) ? ApEl0ReadWrite : ApEl0ReadOnly;
    if (False(perms & Common::MemoryPermission::Execute)) {
        bits |= UserExecuteNever;
    }
    return bits;
}

std::unique_ptr<HvVm> g_vm;
HvVm* g_active_vm{};

} // Anonymous namespace

HvVm* HvVm::Get() {
    return g_active_vm;
}

void HvVm::Deactivate() {
    g_active_vm = nullptr;
}

HvVm* HvVm::Create(System& system) {
    if (!g_vm) {
        g_vm.reset(new HvVm());
        if (!g_vm->Initialize(system)) {
            g_vm.reset();
            return nullptr;
        }
    } else if (!g_vm->MapDram(system)) {
        return nullptr;
    } else {
        g_vm->Reset();
    }
    g_active_vm = g_vm.get();
    return g_active_vm;
}

bool HvVm::Initialize(System& system) {
    uint32_t max_ipa_size{};
    hv_vm_config_t config = hv_vm_config_create();
    if (hv_vm_config_get_max_ipa_size(&max_ipa_size) != HV_SUCCESS || max_ipa_size < 40) {
        LOG_ERROR(Core_ARM, "Hypervisor: 40-bit IPA space is unavailable (max {} bits)",
                  max_ipa_size);
        os_release(config);
        return false;
    }
    hv_vm_config_set_ipa_size(config, 40);
    const hv_return_t created = hv_vm_create(config);
    os_release(config);
    if (created != HV_SUCCESS) {
        LOG_ERROR(Core_ARM,
                  "Hypervisor: hv_vm_create failed ({:#x}); is the app signed with "
                  "the com.apple.security.hypervisor entitlement?",
                  static_cast<u32>(created));
        return false;
    }

    if (!MapDram(system)) {
        return false;
    }

    m_tables = static_cast<u8*>(
        mmap(nullptr, TableRegionSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (m_tables == MAP_FAILED ||
        hv_vm_map(m_tables, TableBaseIpa, TableRegionSize,
                  HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: failed to map the page table region");
        return false;
    }

    Reset();
    LOG_INFO(Core_ARM, "Hypervisor: VM created, {:#x} bytes of DRAM mapped", m_dram_size);
    return true;
}

bool HvVm::MapDram(System& system) {
    // The VM outlives emulation sessions, but DeviceMemory is recreated when the memory layout
    // changes (a Switch 1 title, then a Switch 2 one), so follow its current backing.
    u8* const base = system.DeviceMemory().buffer.BackingBasePointer();
    const u64 size = Common::AlignUp(
        Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(), HostPageSize);
    if (base == m_dram_base && size == m_dram_size) {
        return true;
    }
    if (m_dram_base != nullptr && hv_vm_unmap(0, m_dram_size) != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: failed to unmap the previous DRAM");
        return false;
    }
    m_dram_base = nullptr;
    m_dram_size = 0;
    if (hv_vm_map(base, 0, size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: failed to map {:#x} bytes of DRAM", size);
        return false;
    }
    m_dram_base = base;
    m_dram_size = size;
    return true;
}

void HvVm::Reset() {
    std::scoped_lock lk{m_table_mutex};

    // Drop all tables (new ones are zeroed on allocation) and rebuild the vector page.
    madvise(m_tables, TableRegionSize, MADV_FREE);
    std::memset(m_tables, 0, RootTableOffset + 0x1000);
    for (size_t i = 0; i < 16; i++) {
        std::memcpy(m_tables + i * 0x80, VectorSlot.data(), sizeof(VectorSlot));
    }
    std::memcpy(m_tables + 0x800, FlushStub.data(), sizeof(FlushStub));
    m_next_table_offset = RootTableOffset + 0x1000;

    // The vectors are only visible to EL1.
    u64* entry = EntryFor(VectorsVa, true);
    *entry = TableBaseIpa | DescValid | DescTableOrPage | AttrIndexNormal | ShareInner |
             AccessFlag | ApEl1ReadOnly | UserExecuteNever;

    m_flush_generation.fetch_add(1, std::memory_order_release);
}

u64* HvVm::TableAt(u64 table_ipa) {
    return reinterpret_cast<u64*>(m_tables + (table_ipa - TableBaseIpa));
}

u64* HvVm::EntryFor(u64 vaddr, bool allocate) {
    // 4K granule with a 39-bit address space: L1 (1 GiB) -> L2 (2 MiB) -> L3 (4 KiB).
    u64* table = TableAt(RootTableIpa());
    for (const u32 shift : {30U, 21U}) {
        u64& descriptor = table[(vaddr >> shift) & 511];
        if ((descriptor & DescValid) == 0) {
            if (!allocate) {
                return nullptr;
            }
            if (m_next_table_offset + 0x1000 > TableRegionSize) {
                LOG_CRITICAL(Core_ARM, "Hypervisor: out of page table memory");
                return nullptr;
            }
            const u64 table_ipa = TableBaseIpa + m_next_table_offset;
            m_next_table_offset += 0x1000;
            std::memset(TableAt(table_ipa), 0, 0x1000);
            std::atomic_ref<u64>{descriptor}.store(table_ipa | DescValid | DescTableOrPage,
                                                   std::memory_order_release);
        }
        table = TableAt(descriptor & AddressMask);
    }
    return &table[(vaddr >> 12) & 511];
}

void HvVm::SetPage(u64 vaddr, u64 entry) {
    if (u64* pte = EntryFor(vaddr, true)) {
        std::atomic_ref<u64>{*pte}.store(entry, std::memory_order_release);
    }
}

void HvVm::Map(u64 vaddr, u64 phys_offset, u64 size, Common::MemoryPermission perms) {
    const u64 perm_bits = PermissionBits(perms);
    bool replaced = false;
    {
        std::scoped_lock lk{m_table_mutex};
        for (u64 offset = 0; offset < size; offset += 0x1000) {
            if (const u64* pte = EntryFor(vaddr + offset, false); pte && (*pte & DescValid)) {
                replaced = true;
            }
            SetPage(vaddr + offset, ((phys_offset + offset) & AddressMask) | perm_bits |
                                        DescTableOrPage | AttrIndexNormal | ShareInner |
                                        AccessFlag);
        }
    }
    if (replaced) {
        RequestFlush();
    }
}

void HvVm::Unmap(u64 vaddr, u64 size) {
    {
        std::scoped_lock lk{m_table_mutex};
        for (u64 offset = 0; offset < size; offset += 0x1000) {
            if (u64* pte = EntryFor(vaddr + offset, false)) {
                std::atomic_ref<u64>{*pte}.store(0, std::memory_order_release);
            }
        }
    }
    RequestFlush();
}

void HvVm::Protect(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    const u64 perm_bits = PermissionBits(perms);
    {
        std::scoped_lock lk{m_table_mutex};
        for (u64 offset = 0; offset < size; offset += 0x1000) {
            u64* pte = EntryFor(vaddr + offset, false);
            if (pte == nullptr || (*pte & AddressMask) == 0) {
                continue;
            }
            // Invalid entries keep their output address so a later Protect can restore them.
            const u64 entry = (*pte & ~PermissionMask) | perm_bits;
            std::atomic_ref<u64>{*pte}.store(entry, std::memory_order_release);
        }
    }
    RequestFlush();
}

void HvVm::ProtectAccess(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    u64 access_bits = 0;
    if (True(perms & Common::MemoryPermission::Write)) {
        access_bits = DescValid | ApEl0ReadWrite;
    } else if (True(perms & Common::MemoryPermission::Read)) {
        access_bits = DescValid | ApEl0ReadOnly;
    }
    {
        std::scoped_lock lk{m_table_mutex};
        for (u64 offset = 0; offset < size; offset += 0x1000) {
            u64* pte = EntryFor(vaddr + offset, false);
            if (pte == nullptr || (*pte & AddressMask) == 0) {
                continue;
            }
            const u64 entry = (*pte & ~(DescValid | ApEl0ReadOnly)) | access_bits;
            std::atomic_ref<u64>{*pte}.store(entry, std::memory_order_release);
        }
    }
    RequestFlush();
}

bool HvVm::MapFromPageTable(const Common::PageTable& page_table, u64 vaddr) {
    vaddr &= ~u64{0xFFF};
    if ((vaddr >> page_table.GetAddressSpaceBits()) != 0) {
        return false;
    }
    const auto [pointer, type, block] = page_table.entries[vaddr >> 12].PointerTypeBlock(true);
    if (type == Common::PageType::Unmapped || pointer == 0) {
        return false;
    }
    const u8* host = reinterpret_cast<const u8*>(pointer + vaddr);
    if (host < m_dram_base || host >= m_dram_base + m_dram_size) {
        return false;
    }

    const bool cached = type == Common::PageType::RasterizerCachedMemory;
    std::scoped_lock lk{m_table_mutex};
    if (const u64* pte = EntryFor(vaddr, false); pte != nullptr) {
        // A GPU-tracked page without access was protected on purpose (reactive flushing): mapping
        // it here would let the access through without syncing with the rasterizer.
        if ((*pte & DescValid) != 0 || (cached && (*pte & AddressMask) != 0)) {
            return false;
        }
    }
    // The emulated page table doesn't record permissions. GPU-tracked pages get the access
    // RasterizerMarkRegionCached gives them, so CPU accesses still fault into the rasterizer.
    u64 perm_bits =
        PermissionBits(Common::MemoryPermission::ReadWrite | Common::MemoryPermission::Execute);
    if (cached) {
        perm_bits =
            PermissionBits(Common::MemoryPermission::Read | Common::MemoryPermission::Execute);
        if (Settings::values.use_reactive_flushing.GetValue()) {
            perm_bits &= ~DescValid;
        }
    }
    SetPage(vaddr, (static_cast<u64>(host - m_dram_base) & AddressMask) | perm_bits |
                       DescTableOrPage | AttrIndexNormal | ShareInner | AccessFlag);
    LOG_DEBUG(Core_ARM, "Hypervisor: lazily mapped {:#x}", vaddr);
    return (perm_bits & DescValid) != 0;
}

bool HvVm::Permits(u64 vaddr, bool write) {
    std::scoped_lock lk{m_table_mutex};
    const u64* pte = EntryFor(vaddr, false);
    if (pte == nullptr || (*pte & DescValid) == 0) {
        return false;
    }
    return !write || (*pte & ApEl0ReadOnly) == ApEl0ReadWrite;
}

void HvVm::RequestFlush() {
    m_flush_generation.fetch_add(1, std::memory_order_acq_rel);
    std::scoped_lock lk{m_running_mutex};
    for (size_t i = 0; i < m_vcpus.size(); i++) {
        if (m_running[i]) {
            hv_vcpus_exit(&m_vcpus[i], 1);
        }
    }
}

void HvVm::SetRunning(size_t core, hv_vcpu_t vcpu, bool running) {
    std::scoped_lock lk{m_running_mutex};
    m_vcpus[core] = vcpu;
    m_running[core] = running;
}

} // namespace Core
