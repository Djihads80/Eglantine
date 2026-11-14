// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/settings.h"
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u32 ExceptionClassUnknown = 0x00;
constexpr u32 ExceptionClassWfx = 0x01;
constexpr u32 ExceptionClassSvc64 = 0x15;
constexpr u32 ExceptionClassHvc64 = 0x16;
constexpr u32 ExceptionClassSysReg = 0x18;
constexpr u32 ExceptionClassInstructionAbortLower = 0x20;
constexpr u32 ExceptionClassDataAbortLower = 0x24;
constexpr u32 ExceptionClassSoftwareStepLower = 0x32;
constexpr u32 ExceptionClassBrk64 = 0x3C;

constexpr u64 PstateSoftwareStep = 1ULL << 21;
constexpr u64 MdscrSoftwareStep = 1ULL << 0;

constexpr u64 PstateEl1h = 0x3C5; // EL1h with DAIF masked
constexpr u64 PstateNzcvMask = 0xF0000000;

// SCTLR_EL1: MMU, caches, and EL0 access to DC ZVA, CTR_EL0, WFI/WFE and cache maintenance.
constexpr u64 SctlrEl1 = 0x30D00800ULL | (1ULL << 0) | (1ULL << 2) | (1ULL << 12) | (1ULL << 14) |
                         (1ULL << 15) | (1ULL << 16) | (1ULL << 18) | (1ULL << 26);
// TCR_EL1: T0SZ=25 (39-bit), write-back inner shareable walks, 4K granule, no TTBR1, 40-bit IPA.
constexpr u64 TcrEl1 =
    25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32);
constexpr u64 MairEl1 = 0xFF;        // Attr0: normal memory, write-back
constexpr u64 CpacrEl1 = 3ULL << 20; // FP/SIMD at EL0 and EL1

constexpr u32 ExceptionClass(u64 esr) {
    return static_cast<u32>(esr >> 26) & 0x3F;
}

u64 HostCounter() {
    u64 value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

u64 HostCounterFrequency() {
    u64 value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

// A vCPU can only be destroyed by the thread that created it, and it outlives that thread
// otherwise (the VM has a fixed number of them), so release a thread's vCPUs when it exits.
struct ThreadVcpus {
    std::vector<hv_vcpu_t> vcpus;

    ~ThreadVcpus() {
        for (const hv_vcpu_t vcpu : vcpus) {
            hv_vcpu_destroy(vcpu);
        }
    }
};
thread_local ThreadVcpus t_thread_vcpus;

#define HV_CHECK(call)                                                                             \
    do {                                                                                           \
        if (const hv_return_t r_ = (call); r_ != HV_SUCCESS) {                                     \
            LOG_CRITICAL(Core_ARM, "Hypervisor: {} failed ({:#x})", #call, static_cast<u32>(r_));  \
        }                                                                                          \
    } while (0)

} // Anonymous namespace

ArmHypervisor::ArmHypervisor(System& system, HvVm& vm, std::size_t core_index)
    : ArmInterface{true}, m_system{system}, m_vm{vm}, m_core_index{core_index} {
    // Wake each vCPU every millisecond so an interrupt that races vCPU entry is never lost.
    m_heartbeat_ticks = HostCounterFrequency() / 1000;
}

ArmHypervisor::~ArmHypervisor() {
    // The vCPU itself is destroyed when its owning thread exits.
    m_vm.SetRunning(m_core_index, m_vcpu, false);
}

bool ArmHypervisor::EnsureVcpu() {
    if (m_vcpu_created) {
        if (!pthread_equal(m_owner_thread, pthread_self())) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: core {} vCPU used from a foreign thread",
                         m_core_index);
            return false;
        }
        return true;
    }

    if (hv_vcpu_create(&m_vcpu, &m_exit, nullptr) != HV_SUCCESS) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: failed to create vCPU for core {}", m_core_index);
        return false;
    }
    m_vcpu_created = true;
    m_owner_thread = pthread_self();
    t_thread_vcpus.vcpus.push_back(m_vcpu);

    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MAIR_EL1, MairEl1));
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TCR_EL1, TcrEl1));
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TTBR0_EL1, m_vm.RootTableIpa()));
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_VBAR_EL1, HvVm::VectorsVa));
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CPACR_EL1, CpacrEl1));
    // Trap EL0 counter and frequency reads so they can run at the guest's frequency.
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTKCTL_EL1, 0));
    HV_CHECK(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SCTLR_EL1, SctlrEl1));
    // Route software step (used for GPU-tracked accesses) and BRK straight to the host.
    HV_CHECK(hv_vcpu_set_trap_debug_exceptions(m_vcpu, true));

    m_flushed_generation = 0;
    LOG_INFO(Core_ARM, "Hypervisor: created vCPU for core {}", m_core_index);
    return true;
}

void ArmHypervisor::LoadVcpuContext() {
    for (int i = 0; i < 29; i++) {
        hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), m_ctx.r[i]);
    }
    hv_vcpu_set_reg(m_vcpu, HV_REG_FP, m_ctx.fp);
    hv_vcpu_set_reg(m_vcpu, HV_REG_LR, m_ctx.lr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_PC, m_ctx.pc);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, m_ctx.pstate & PstateNzcvMask);
    hv_vcpu_set_reg(m_vcpu, HV_REG_FPCR, m_ctx.fpcr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_FPSR, m_ctx.fpsr);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, m_ctx.sp);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL0, m_ctx.tpidr);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, &m_ctx.v[i], sizeof(value));
        hv_vcpu_set_simd_fp_reg(m_vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                value);
    }
}

void ArmHypervisor::ReadVcpuContext(Kernel::Svc::ThreadContext& ctx) const {
    for (int i = 0; i < 29; i++) {
        hv_vcpu_get_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), &ctx.r[i]);
    }
    u64 value{};
    hv_vcpu_get_reg(m_vcpu, HV_REG_FP, &ctx.fp);
    hv_vcpu_get_reg(m_vcpu, HV_REG_LR, &ctx.lr);
    hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &ctx.pc);
    hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &value);
    ctx.pstate = static_cast<u32>(value & PstateNzcvMask);
    hv_vcpu_get_reg(m_vcpu, HV_REG_FPCR, &value);
    ctx.fpcr = static_cast<u32>(value);
    hv_vcpu_get_reg(m_vcpu, HV_REG_FPSR, &value);
    ctx.fpsr = static_cast<u32>(value);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, &ctx.sp);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL0, &ctx.tpidr);
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t q;
        hv_vcpu_get_simd_fp_reg(m_vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), &q);
        std::memcpy(&ctx.v[i], &q, sizeof(q));
    }
}

bool ArmHypervisor::VcpuHoldsContext() const {
    if (!m_vcpu_has_context) {
        return false;
    }
    if (!pthread_equal(m_owner_thread, pthread_self())) {
        LOG_ERROR(Core_ARM, "Hypervisor: core {} context accessed from a foreign thread",
                  m_core_index);
        return false;
    }
    return true;
}

void ArmHypervisor::ArmHeartbeat() {
    u64 offset{};
    hv_vcpu_get_vtimer_offset(m_vcpu, &offset);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTV_CVAL_EL0,
                        HostCounter() - offset + m_heartbeat_ticks);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTV_CTL_EL0, 1);
    hv_vcpu_set_vtimer_mask(m_vcpu, false);
}

bool ArmHypervisor::RunFlushStub() {
    u64 pc{}, cpsr{}, x0{}, mdscr{};
    hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_reg(m_vcpu, HV_REG_X0, &x0);
    // A pending single step would trap on the first stub instruction.
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_MDSCR_EL1, &mdscr);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MDSCR_EL1, 0);
    hv_vcpu_set_reg(m_vcpu, HV_REG_PC, HvVm::FlushStubVa);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, PstateEl1h);

    bool done = false;
    for (int attempt = 0; attempt < 64 && !done; attempt++) {
        if (hv_vcpu_run(m_vcpu) != HV_SUCCESS) {
            break;
        }
        if (m_exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            ArmHeartbeat();
        } else if (m_exit->reason == HV_EXIT_REASON_EXCEPTION) {
            const u64 syndrome = m_exit->exception.syndrome;
            done = ExceptionClass(syndrome) == ExceptionClassHvc64 && (syndrome & 0xFFFF) == 1;
            if (!done) {
                LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit {:#x} in flush stub", syndrome);
                break;
            }
        }
    }

    hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, cpsr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_X0, x0);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MDSCR_EL1, mdscr);
    return done;
}

bool ArmHypervisor::EmulateSystemRegister(u64 esr) {
    const bool is_read = (esr & 1) != 0;
    const u32 rt = (esr >> 5) & 0x1F;
    const u32 crm = (esr >> 1) & 0xF;
    const u32 crn = (esr >> 10) & 0xF;
    const u32 op1 = (esr >> 14) & 0x7;
    const u32 op2 = (esr >> 17) & 0x7;
    const u32 op0 = (esr >> 20) & 0x3;

    if (!is_read || op0 != 3 || op1 != 3 || crn != 14 || crm != 0) {
        LOG_WARNING(Core_ARM, "Hypervisor: unhandled system register access {:#x}", esr);
        return false;
    }

    u64 value{};
    switch (op2) {
    case 0: // CNTFRQ_EL0
        value = Common::g_wall_clock.GetGuestCNTFRQ();
        break;
    case 1: // CNTPCT_EL0
    case 2: // CNTVCT_EL0
    case 5: // CNTPCTSS_EL0
    case 6: // CNTVCTSS_EL0
        value = static_cast<u64>(Common::g_wall_clock.GetCNTPCT());
        break;
    default:
        LOG_WARNING(Core_ARM, "Hypervisor: unhandled counter register access {:#x}", esr);
        return false;
    }
    if (rt != 31) {
        hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + rt), value);
    }
    return true;
}

static bool IsMapped(Kernel::KProcess* process, u64 vaddr) {
    const auto& page_table = process->GetPageTable().GetImpl();
    if ((vaddr >> page_table.GetAddressSpaceBits()) != 0) {
        return false;
    }
    return page_table.entries[vaddr >> Memory::YUZU_PAGEBITS].Type() != Common::PageType::Unmapped;
}

static bool IsRasterizerCached(Kernel::KProcess* process, u64 vaddr) {
    const auto& page_table = process->GetPageTable().GetImpl();
    if ((vaddr >> page_table.GetAddressSpaceBits()) != 0) {
        return false;
    }
    return page_table.entries[vaddr >> Memory::YUZU_PAGEBITS].Type() ==
           Common::PageType::RasterizerCachedMemory;
}

void ArmHypervisor::FinishTrackedAccess() {
    if (!m_stepping) {
        return;
    }
    m_stepping = false;
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MDSCR_EL1, 0);
    // Restore the protection RasterizerMarkRegionCached applies, unless the GPU released the page
    // in the meantime.
    if (IsRasterizerCached(m_step_process, m_step_page)) {
        const auto perm = Settings::values.use_reactive_flushing.GetValue()
                              ? Common::MemoryPermission{}
                              : Common::MemoryPermission::Read;
        m_vm.ProtectAccess(m_step_page, Memory::YUZU_PAGESIZE, perm);
    }
}

bool ArmHypervisor::EmulateMemoryTagging(Kernel::KProcess* process, u64 pc) {
    // Apple Silicon has no MTE, so its instructions are undefined there. The emulated memory
    // keeps no tags either: tag stores only write back their base (STZG/STZ2G also zero the
    // granules, STGP stores its pair) and LDG reads tag 0, matching Dynarmic.
    auto& memory = process->GetMemory();
    const u32 insn = memory.Read32(pc);
    const u32 rt = insn & 0x1F;
    const u32 rn = (insn >> 5) & 0x1F;
    const u64 base = GetBaseRegister(rn);
    const auto get_data = [&](u32 index) { return index == 31 ? 0 : GetBaseRegister(index); };
    // Indexing mode, as both encodings number it: 1 post-index, 2 signed offset, 3 pre-index.
    const auto finish = [&](u32 mode, u64 offset) {
        if (mode != 2) {
            SetBaseRegister(rn, base + offset);
        }
        return mode == 1 ? base : base + offset;
    };

    if ((insn & 0xFF200000) == 0xD9200000) {
        // STG, STZG, ST2G, STZ2G, LDG
        const u32 opc = (insn >> 22) & 3;
        const u32 mode = (insn >> 10) & 3;
        const u64 offset = static_cast<u64>(static_cast<s64>(static_cast<u64>(insn) << 43) >> 55)
                           << 4;
        if (mode == 0) {
            if (opc != 1) {
                return false; // STZGM, STGM, LDGM: EL1 only
            }
            if (rt != 31) {
                SetBaseRegister(rt, get_data(rt) & ~(u64{0xF} << 56));
            }
            return true;
        }
        const u64 address = finish(mode, offset);
        if ((opc & 1) != 0) {
            static constexpr std::array<u8, 32> zeros{};
            memory.WriteBlock(address, zeros.data(), opc == 3 ? 32 : 16);
        }
        return true;
    }
    if ((insn & 0xFE400000) == 0x68000000 && ((insn >> 23) & 3) != 0) {
        // STGP
        const u32 rt2 = (insn >> 10) & 0x1F;
        const u64 offset = static_cast<u64>(static_cast<s64>(static_cast<u64>(insn) << 42) >> 57)
                           << 4;
        const u64 address = finish((insn >> 23) & 3, offset);
        memory.Write64(address, get_data(rt));
        memory.Write64(address + 8, get_data(rt2));
        return true;
    }
    return false;
}

u64 ArmHypervisor::GetBaseRegister(u32 index) {
    u64 value{};
    if (index == 31) {
        hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, &value);
    } else {
        hv_vcpu_get_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + index), &value);
    }
    return value;
}

void ArmHypervisor::SetBaseRegister(u32 index, u64 value) {
    if (index == 31) {
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, value);
    } else {
        hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + index), value);
    }
}

bool ArmHypervisor::EmulateTrackedAccess(Kernel::KProcess* process, u64 pc) {
    auto& memory = process->GetMemory();
    const u32 insn = memory.Read32(pc);
    const u32 rt = insn & 0x1F;
    const u32 rn = (insn >> 5) & 0x1F;

    u32 size_log2{}; // bytes per register, log2
    bool is_load{};
    bool is_vector{};
    bool sign_extend{};
    bool sign_to_64{};
    bool is_pair{};
    u32 rt2{};
    u64 address{};
    std::optional<u64> writeback;

    const auto sign_extend_imm = [](u32 value, u32 bits) {
        return static_cast<u64>(static_cast<s64>(static_cast<u64>(value) << (64 - bits)) >>
                                (64 - bits));
    };
    // Size, direction and signedness of the single-register load/store classes.
    const auto decode_single = [&]() {
        const u32 size = insn >> 30;
        const u32 opc = (insn >> 22) & 3;
        is_vector = ((insn >> 26) & 1) != 0;
        if (is_vector) {
            size_log2 = ((opc & 2) << 1) | size;
            is_load = (opc & 1) != 0;
            return size_log2 <= 4;
        }
        size_log2 = size;
        is_load = opc != 0;
        sign_extend = opc >= 2;
        sign_to_64 = opc == 2;
        // PRFM and the unallocated signed forms.
        return !(size == 3 && opc >= 2) && !(size == 2 && opc == 3);
    };

    if ((insn & 0x3B000000) == 0x39000000) {
        // LDR/STR (unsigned immediate)
        if (!decode_single()) {
            return false;
        }
        address = GetBaseRegister(rn) + (static_cast<u64>((insn >> 10) & 0xFFF) << size_log2);
    } else if ((insn & 0x3B200000) == 0x38000000) {
        // LDUR/STUR, LDTR/STTR, and the pre/post-indexed forms
        if (!decode_single()) {
            return false;
        }
        const u64 base = GetBaseRegister(rn);
        const u64 offset = sign_extend_imm((insn >> 12) & 0x1FF, 9);
        switch ((insn >> 10) & 3) {
        case 1: // post-index
            address = base;
            writeback = base + offset;
            break;
        case 3: // pre-index
            address = base + offset;
            writeback = address;
            break;
        default:
            address = base + offset;
            break;
        }
    } else if ((insn & 0x3B200C00) == 0x38200800) {
        // LDR/STR (register offset)
        if (!decode_single()) {
            return false;
        }
        const u32 rm = (insn >> 16) & 0x1F;
        const u32 option = (insn >> 13) & 7;
        u64 offset = rm == 31 ? 0 : GetBaseRegister(rm);
        switch (option) {
        case 2: // UXTW
            offset = static_cast<u32>(offset);
            break;
        case 6: // SXTW
            offset = static_cast<u64>(static_cast<s64>(static_cast<s32>(offset)));
            break;
        case 3: // LSL
        case 7: // SXTX
            break;
        default:
            return false;
        }
        if (((insn >> 12) & 1) != 0) {
            offset <<= size_log2;
        }
        address = GetBaseRegister(rn) + offset;
    } else if ((insn & 0x3A000000) == 0x28000000) {
        // LDP/STP/LDNP/STNP/LDPSW
        const u32 opc = insn >> 30;
        is_pair = true;
        is_vector = ((insn >> 26) & 1) != 0;
        is_load = ((insn >> 22) & 1) != 0;
        rt2 = (insn >> 10) & 0x1F;
        if (is_vector) {
            if (opc == 3) {
                return false;
            }
            size_log2 = 2 + opc;
        } else {
            if (opc == 3 || (opc == 1 && !is_load)) {
                return false;
            }
            size_log2 = opc == 2 ? 3 : 2;
            sign_extend = opc == 1;
            sign_to_64 = sign_extend;
        }
        const u64 base = GetBaseRegister(rn);
        const u64 offset = sign_extend_imm((insn >> 15) & 0x7F, 7) << size_log2;
        switch ((insn >> 23) & 3) {
        case 1: // post-index
            address = base;
            writeback = base + offset;
            break;
        case 3: // pre-index
            address = base + offset;
            writeback = address;
            break;
        default:
            address = base + offset;
            break;
        }
    } else if ((insn & 0x3FFFFC00) == 0x08DFFC00 || (insn & 0x3FFFFC00) == 0x38BFC000 ||
               (insn & 0x3FFFFC00) == 0x089FFC00) {
        // LDAR, LDAPR, STLR
        size_log2 = insn >> 30;
        is_load = (insn & 0x3FFFFC00) != 0x089FFC00;
        address = GetBaseRegister(rn);
    } else {
        return false;
    }

    const size_t size = size_t{1} << size_log2;
    const auto vector_register = [](u32 index) {
        return static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + index);
    };
    const auto read_register = [&](u32 index, u8* out) {
        if (is_vector) {
            hv_simd_fp_uchar16_t value;
            hv_vcpu_get_simd_fp_reg(m_vcpu, vector_register(index), &value);
            std::memcpy(out, &value, size);
        } else {
            const u64 value = index == 31 ? 0 : GetBaseRegister(index);
            std::memcpy(out, &value, size);
        }
    };
    const auto write_register = [&](u32 index, const u8* in) {
        if (is_vector) {
            std::array<u8, 16> bytes{};
            std::memcpy(bytes.data(), in, size);
            hv_simd_fp_uchar16_t value;
            std::memcpy(&value, bytes.data(), sizeof(value));
            hv_vcpu_set_simd_fp_reg(m_vcpu, vector_register(index), value);
            return;
        }
        if (index == 31) {
            return;
        }
        u64 value{};
        std::memcpy(&value, in, size);
        if (sign_extend && size < 8) {
            value = sign_extend_imm(static_cast<u32>(value), static_cast<u32>(size * 8));
            if (!sign_to_64) {
                value = static_cast<u32>(value);
            }
        }
        SetBaseRegister(index, value);
    };

    std::array<u8, 32> data{};
    const size_t total = is_pair ? size * 2 : size;
    if (is_load) {
        memory.ReadBlock(address, data.data(), total);
        write_register(rt, data.data());
        if (is_pair) {
            write_register(rt2, data.data() + size);
        }
    } else {
        read_register(rt, data.data());
        if (is_pair) {
            read_register(rt2, data.data() + size);
        }
        memory.WriteBlock(address, data.data(), total);
    }
    if (writeback) {
        SetBaseRegister(rn, *writeback);
    }
    return true;
}

HaltReason ArmHypervisor::HandleGuestException(Kernel::KThread* thread) {
    u64 guest_x0{};
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL1, &guest_x0);
    hv_vcpu_set_reg(m_vcpu, HV_REG_X0, guest_x0);

    u64 esr{}, elr{}, far{}, spsr{};
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_ESR_EL1, &esr);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_ELR_EL1, &elr);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_FAR_EL1, &far);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_SPSR_EL1, &spsr);

    // Emulate the exception return from the host.
    const auto return_to = [&](u64 pc) {
        HV_CHECK(hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc));
        HV_CHECK(hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, spsr));
    };

    switch (ExceptionClass(esr)) {
    case ExceptionClassSvc64:
        m_svc = static_cast<u32>(esr & 0xFFFF);
        return_to(elr);
        return HaltReason::SupervisorCall;

    case ExceptionClassSysReg:
        return_to(elr + 4);
        if (!EmulateSystemRegister(esr)) {
            // Unknown registers read as zero.
            const u32 rt = (esr >> 5) & 0x1F;
            if ((esr & 1) != 0 && rt != 31) {
                hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + rt), 0);
            }
        }
        return {};

    case ExceptionClassDataAbortLower:
    case ExceptionClassInstructionAbortLower: {
        const bool is_prefetch = ExceptionClass(esr) == ExceptionClassInstructionAbortLower;
        auto* process = thread->GetOwnerProcess();
        const u64 page = far & ~u64{0xFFF};

        const bool repeated = elr == m_last_fault_pc && far == m_last_fault_address;
        m_fault_repeat = repeated ? m_fault_repeat + 1 : 0;
        m_last_fault_pc = elr;
        m_last_fault_address = far;

        // Retry when the page was missing from the stage-1 tables.
        const bool is_write = !is_prefetch && (esr & (1ULL << 6)) != 0;
        // A stale TLB entry (the page was unprotected after this vCPU cached it) only needs a
        // flush.
        bool retry = m_fault_repeat < 64 &&
                     (m_vm.MapFromPageTable(process->GetPageTable().GetImpl(), page) ||
                      m_vm.Permits(page, is_write));
        if (!retry && !is_prefetch && IsRasterizerCached(process, page)) {
            // GPU-tracked memory: like Dynarmic's slow path, perform the access through the
            // emulated memory, so reads download GPU-written data and writes notify the GPU.
            FinishTrackedAccess();
            if (EmulateTrackedAccess(process, elr)) {
                return_to(elr + 4);
                return {};
            }
        }
        // Accesses EmulateTrackedAccess doesn't decode: sync with the rasterizer and let the access
        // through. The GPU thread can protect the page again before the retried access lands, so
        // allow more attempts here and let it settle between them.
        if (!retry && !is_prefetch && m_fault_repeat < 64 && IsRasterizerCached(process, page)) {
            if (m_fault_repeat >= 4) {
                std::this_thread::yield();
            }
            auto& memory = process->GetMemory();
            if (is_write) {
                memory.InvalidateNCE(page, Memory::YUZU_PAGESIZE);
            } else {
                memory.DownloadRasterizerRegion(page, Memory::YUZU_PAGESIZE);
            }
            // Open the page for this one access, then single-step it so the protection is
            // restored right after (every CPU write keeps notifying the GPU, as in Dynarmic).
            FinishTrackedAccess();
            m_vm.ProtectAccess(page, Memory::YUZU_PAGESIZE,
                               is_write ? Common::MemoryPermission::ReadWrite
                                        : Common::MemoryPermission::Read);
            m_step_process = process;
            m_step_page = page;
            m_stepping = true;
            hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MDSCR_EL1, MdscrSoftwareStep);
            spsr |= PstateSoftwareStep;
            retry = true;
        }
        // The GPU thread can track or untrack the page between the checks above (protected and
        // tracked when mapping was tried, untracked by the time it was looked up): every check
        // failed for a mapped page and the access was skipped, so the game read a stale value.
        // Retry while the page table says it is mapped; the state settles by the next fault.
        if (!retry && m_fault_repeat < 64 && IsMapped(process, page)) {
            retry = true;
        }
        if (retry) {
            return_to(elr);
            if (!RunFlushStub()) {
                return HaltReason::PrefetchAbort;
            }
            return {};
        }

        if (is_prefetch) {
            LOG_ERROR(Core_ARM, "Hypervisor: instruction abort at {:#x} (esr={:#x})", elr, esr);
            return_to(elr);
            return HaltReason::PrefetchAbort;
        }

        // Like NCE, skip data accesses that can't be satisfied instead of halting the game.
        const auto& page_table = process->GetPageTable().GetImpl();
        const u32 page_type = (page >> page_table.GetAddressSpaceBits()) == 0
                                  ? static_cast<u32>(page_table.entries[page >> 12].Type())
                                  : ~0U;
        LOG_ERROR(Core_ARM,
                  "Hypervisor: unhandled data abort at pc={:#x} address={:#x} "
                  "(esr={:#x}, page type {}, attempt {}), skipping",
                  elr, far, esr, page_type, m_fault_repeat);
        return_to(elr + 4);
        return {};
    }

    case ExceptionClassBrk64:
        return_to(elr);
        return HaltReason::InstructionBreakpoint;

    case ExceptionClassUnknown:
        if (EmulateMemoryTagging(thread->GetOwnerProcess(), elr)) {
            return_to(elr + 4);
            return {};
        }
        [[fallthrough]];
    default:
        LOG_ERROR(Core_ARM,
                  "Hypervisor: unhandled guest exception at {:#x} (esr={:#x}, "
                  "far={:#x})",
                  elr, esr, far);
        return_to(elr);
        return HaltReason::PrefetchAbort;
    }
}

HaltReason ArmHypervisor::RunThread(Kernel::KThread* thread) {
    if (const auto hr = static_cast<HaltReason>(m_halt.exchange(0)); True(hr)) {
        return hr;
    }
    if (!EnsureVcpu()) {
        return HaltReason::PrefetchAbort;
    }

    // The vCPU keeps the guest registers between runs; only a context switch reloads them all.
    if (!m_vcpu_has_context) {
        LoadVcpuContext();
        m_vcpu_has_context = true;
        m_tpidrro_dirty = false;
    } else if (m_tpidrro_dirty) {
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
        m_tpidrro_dirty = false;
    }
    ArmHeartbeat();
    m_vm.SetRunning(m_core_index, m_vcpu, true);
    m_running.store(true, std::memory_order_release);

    HaltReason hr{};
    bool in_vector = false;
    while (!True(hr)) {
        // An exit can land between taking a guest exception and its "hvc"; finish that first so
        // the exception isn't lost and EL1 state never leaks into the guest context.
        if (!std::exchange(in_vector, false)) {
            if (const u64 pending = m_halt.exchange(0); pending != 0) {
                hr |= static_cast<HaltReason>(pending);
                break;
            }
            if (const u64 generation = m_vm.FlushGeneration(); generation != m_flushed_generation) {
                m_flushed_generation = generation;
                if (!RunFlushStub()) {
                    hr |= HaltReason::PrefetchAbort;
                    break;
                }
            }
        }

        if (hv_vcpu_run(m_vcpu) != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: hv_vcpu_run failed on core {}", m_core_index);
            hr |= HaltReason::PrefetchAbort;
            break;
        }

        const auto check_in_vector = [&] {
            u64 cpsr{};
            hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr);
            in_vector = (cpsr & 0xF) != 0;
            if (!in_vector) {
                return;
            }
            // A cancel can be reported after the vector's "hvc" already executed; rerun the hvc so
            // the exception (still held in ESR/ELR/SPSR, x0 in TPIDR_EL1) is delivered.
            u64 pc{};
            hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
            if (pc >= HvVm::VectorsVa && pc < HvVm::FlushStubVa &&
                (pc & 0x7F) > HvVm::VectorHvcOffset) {
                hv_vcpu_set_reg(m_vcpu, HV_REG_PC, (pc & ~u64{0x7F}) + HvVm::VectorHvcOffset);
            }
        };

        switch (m_exit->reason) {
        case HV_EXIT_REASON_CANCELED:
            // Requested by SignalInterrupt or a flush; both are handled at the top of the loop.
            check_in_vector();
            break;
        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            ArmHeartbeat();
            check_in_vector();
            break;
        case HV_EXIT_REASON_EXCEPTION: {
            const u64 syndrome = m_exit->exception.syndrome;
            const u32 ec = ExceptionClass(syndrome);
            if (ec == ExceptionClassHvc64 && (syndrome & 0xFFFF) == 0) {
                hr |= HandleGuestException(thread);
            } else if (ec == ExceptionClassSoftwareStepLower) {
                FinishTrackedAccess();
                // A stepped access that faults again completes the step on the vector's first
                // instruction: deliver that exception before acting on an interrupt, or the
                // thread is saved with the vector's PC and resumes there at EL0.
                check_in_vector();
            } else if (ec == ExceptionClassBrk64) {
                hr |= HaltReason::InstructionBreakpoint;
            } else if (ec == ExceptionClassWfx) {
                u64 pc{};
                hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
                hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc + 4);
            } else {
                LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit syndrome {:#x} (ipa={:#x})",
                             syndrome, m_exit->exception.physical_address);
                hr |= HaltReason::PrefetchAbort;
            }
            break;
        }
        default:
            LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit reason {}",
                         static_cast<u32>(m_exit->reason));
            hr |= HaltReason::PrefetchAbort;
            break;
        }
    }

    m_running.store(false, std::memory_order_release);
    m_vm.SetRunning(m_core_index, m_vcpu, false);
    // An unfinished step means the access didn't run yet; it faults again when resumed.
    FinishTrackedAccess();
    return hr | static_cast<HaltReason>(m_halt.exchange(0));
}

HaltReason ArmHypervisor::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

// The context accessors are only called from the core's own thread (scheduler and SVC dispatch),
// which owns the vCPU, so they read and write its registers directly while it holds them.
void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    if (VcpuHoldsContext()) {
        ReadVcpuContext(ctx);
        return;
    }
    ctx = m_ctx;
}

void ArmHypervisor::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    m_ctx = ctx;
    m_vcpu_has_context = false;
}

void ArmHypervisor::SetTpidrroEl0(u64 value) {
    m_tpidrro_el0 = value;
    m_tpidrro_dirty = true;
}

void ArmHypervisor::GetSvcArguments(std::span<uint64_t, 8> args) const {
    if (VcpuHoldsContext()) {
        for (size_t i = 0; i < 8; i++) {
            hv_vcpu_get_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), &args[i]);
        }
        return;
    }
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_ctx.r[i];
    }
}

void ArmHypervisor::SetSvcArguments(std::span<const uint64_t, 8> args) {
    if (VcpuHoldsContext()) {
        for (size_t i = 0; i < 8; i++) {
            hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), args[i]);
        }
        return;
    }
    for (size_t i = 0; i < 8; i++) {
        m_ctx.r[i] = args[i];
    }
}

u32 ArmHypervisor::GetSvcNumber() const {
    return m_svc;
}

void ArmHypervisor::SignalInterrupt(Kernel::KThread* thread) {
    m_halt.fetch_or(static_cast<u64>(HaltReason::BreakLoop));
    if (m_running.load(std::memory_order_acquire)) {
        hv_vcpus_exit(&m_vcpu, 1);
    }
}

void ArmHypervisor::ClearInstructionCache() {
    m_vm.RequestFlush();
}

void ArmHypervisor::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_vm.RequestFlush();
}

} // namespace Core
