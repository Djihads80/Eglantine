// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>

#include "core/arm/arm_interface.h"
#include "core/arm/nce/guest_context.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

class System;

class ArmNce final : public ArmInterface {
public:
    ArmNce(System& system, bool uses_wall_clock, std::size_t core_index);
    ~ArmNce() override;

    void Initialize() override;

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

    void LockThread(Kernel::KThread* thread) override;
    void UnlockThread(Kernel::KThread* thread) override;

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

private:
    // Assembly definitions.
    static HaltReason ReturnToRunCodeByTrampoline(void* tpidr, GuestContext* ctx,
                                                  u64 trampoline_addr);
    static HaltReason ReturnToRunCodeByExceptionLevelChange(int tid, void* tpidr);

    static void ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void* info,
                                                                   void* raw_context);
    static void BreakFromRunCodeSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAlignmentFaultSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAccessFaultSignalHandler(int sig, void* info, void* raw_context);
    static void GuestIllegalInstructionSignalHandler(int sig, void* info, void* raw_context);

    static void LockThreadParameters(void* tpidr);
    static void UnlockThreadParameters(void* tpidr);

private:
    // C++ implementation functions for assembly definitions.
    static void* RestoreGuestContext(void* raw_context);
    static void SaveGuestContext(GuestContext* ctx, void* raw_context);
    static bool HandleFailedGuestFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAlignmentFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAccessFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestIllegalInstruction(GuestContext* ctx, void* info, void* raw_context);
    static void HandleHostAlignmentFault(int sig, void* info, void* raw_context);
    static void HandleHostAccessFault(int sig, void* info, void* raw_context);
    static void HandleHostIllegalInstruction(int sig, void* info, void* raw_context);

public:
    Core::System& m_system;

    // Members set on initialization.
    std::size_t m_core_index{};
    pid_t m_thread_id{-1};

    // Core context.
    GuestContext m_guest_ctx{};
    Kernel::KThread* m_running_thread{};

    // Stack for signal processing.
    std::unique_ptr<u8[]> m_stack{};

    // Diagnostics (written only by this core's thread, in its fault handler): the guest access
    // fault that keeps coming back at the same PC and address.
    u64 m_repeat_fault_pc{};
    u64 m_repeat_fault_address{};
    u64 m_repeat_fault_count{};

    // Fault handler state for GPU-cached page writes (this core's thread only): the previous
    // faulting page, and the stores emulated on recent pages (EDEN_NCE_EMULATE_WRITES).
    u64 m_last_raster_page{};
    struct EmulatedPage {
        u64 page;
        u32 stores;
    };
    std::array<EmulatedPage, 4> m_emulated_pages{};
    u32 m_emulated_next{};

    /// Whether another store to `page` may be emulated (at most `budget` while it stays recent).
    bool TakeEmulateBudget(u64 page, u32 budget);
};

} // namespace Core
