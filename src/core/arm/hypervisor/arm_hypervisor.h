// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include <Hypervisor/Hypervisor.h>
#include <pthread.h>

#include "core/arm/arm_interface.h"
#include "core/hle/kernel/svc_types.h"

namespace Core {

class HvVm;
class System;

/// Runs AArch64 application code natively in a Hypervisor.framework vCPU (Apple Silicon).
/// Guest code executes at EL0; every exception it raises is routed to the host through EL1
/// vectors that issue "hvc", so no code patching is needed.
class ArmHypervisor final : public ArmInterface {
public:
    ArmHypervisor(System& system, HvVm& vm, std::size_t core_index);
    ~ArmHypervisor() override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }
    void RewindBreakpointInstruction() override {}

private:
    bool EnsureVcpu();
    void LoadVcpuContext();
    void ReadVcpuContext(Kernel::Svc::ThreadContext& ctx) const;
    bool VcpuHoldsContext() const;
    bool RunFlushStub();
    void ArmHeartbeat();
    bool EmulateSystemRegister(u64 esr);
    HaltReason HandleGuestException(Kernel::KThread* thread);
    void FinishTrackedAccess();
    /// Performs a load or store to GPU-tracked memory through the emulated memory, as Dynarmic
    /// does. Returns false for instructions it doesn't decode.
    bool EmulateTrackedAccess(Kernel::KProcess* process, u64 pc);
    /// Emulates the MTE instructions (STG, STZG, ST2G, STZ2G, STGP, LDG). Returns false for others.
    bool EmulateMemoryTagging(Kernel::KProcess* process, u64 pc);
    u64 GetBaseRegister(u32 index);
    void SetBaseRegister(u32 index, u64 value);

    System& m_system;
    HvVm& m_vm;
    std::size_t m_core_index;

    hv_vcpu_t m_vcpu{};
    hv_vcpu_exit_t* m_exit{};
    bool m_vcpu_created{};
    pthread_t m_owner_thread{};
    u64 m_flushed_generation{};
    u64 m_heartbeat_ticks{};

    // Guest registers. Once a thread has run, the vCPU holds the current values (m_ctx is only
    // the last context the kernel set) until SetContext switches to another thread.
    Kernel::Svc::ThreadContext m_ctx{};
    u64 m_tpidrro_el0{};
    bool m_vcpu_has_context{};
    bool m_tpidrro_dirty{};
    u32 m_svc{};

    std::atomic<u64> m_halt{};
    std::atomic<bool> m_running{};

    u64 m_last_fault_pc{};
    u64 m_last_fault_address{};
    u32 m_fault_repeat{};

    // GPU-tracked page whose faulting access is being single-stepped with access granted.
    Kernel::KProcess* m_step_process{};
    u64 m_step_page{};
    bool m_stepping{};
};

} // namespace Core
