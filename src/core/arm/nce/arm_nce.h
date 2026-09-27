// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include "core/arm/arm_interface.h"
#include "core/arm/nce/guest_context.h"

#ifdef _WIN32
#include <winternl.h>
#endif

#define SpinLockLocked 0
#define SpinLockUnlocked 1

namespace Core::Memory {
class Memory;
}

namespace Core {

class System;

#ifdef __APPLE__
// TLS index for NativeExecutionParameters in pthreads.
// This value is actually reserved for old versions of iOSSimulator, however we aren't iOSSimulator,
// so we can manually initialize and use it.
// https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/private/pthread/tsd_private.h#L241-L245
constexpr pthread_key_t ContextKey = 210;
#elif _WIN32

#define ExceptionLevelChangeSignal 0xE0000001

inline u32 ContextKey = TlsAlloc();
inline u32 NCEStorage = TlsAlloc();
constexpr u64 TlsSlots = offsetof(TEB, TlsSlots);
#endif


struct NativeExecutionParameters {

#if defined(__APPLE__) || defined(_WIN32)
    // Are we in actual guest code?
    // TODO: make this and is_running the same
    bool is_actually_running{};
#endif

    // Are we in any stage of performing guest operations?
    bool is_running{};
    u32 magic{Common::MakeMagic('Y', 'U', 'Z', 'U')};
    std::atomic<u32> lock{1};
    u64 tpidr_el0{};
    u64 tpidrro_el0{};
    GuestContext* native_context{};

#ifdef _WIN32
    void* guest_stack_base;
    void* guest_stack_limit;
    void* host_stack_base;
    void* host_stack_limit;
#endif
};

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
    void InvalidateCacheRange(u64 addr, std::size_t size) override;
    void ClearInstructionCache() override;

    void LockThread(Kernel::KThread* thread) override;
    void UnlockThread(Kernel::KThread* thread) override;

#ifdef __linux__
    typedef pid_t thread_id;
    static constexpr thread_id NULL_THREAD_ID = -1;
#elif __APPLE__
    typedef mach_port_t thread_id;
    static constexpr thread_id NULL_THREAD_ID = -1U;
#elif _WIN32
    typedef HANDLE thread_id;
    static constexpr thread_id NULL_THREAD_ID = nullptr;
#endif

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

private:
    // Only confirmed to be valid on Apple systems.
    static NativeExecutionParameters* GetGuestParameters();

    static HaltReason ReturnToRunCodeByTrampoline(NativeExecutionParameters* tpidr, u64 trampoline_addr);
    static HaltReason ReturnToRunCodeByExceptionLevelChange(thread_id tid, NativeExecutionParameters* tpidr);

    static void ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void* info,
                                                                   void* raw_context);
    static void BreakFromRunCodeSignalHandler(int sig, void* info, void* raw_context);
    static bool GuestMemoryFaultSignalHandler(NativeExecutionParameters* nep, int sig, u64 fault_addr, void* raw_context);
    static bool HandleFailedGuestFault(GuestContext* ctx, u64 fault_addr, void* raw_context);
#ifdef _WIN32
    static LONG VectoredExceptionHandler(PEXCEPTION_POINTERS info);
#else
    static void POSIXExceptionHandler(int sig, void* info, void* raw_context);
#endif

    static void LockThreadParameters(NativeExecutionParameters* tpidr);
    static void UnlockThreadParameters(NativeExecutionParameters* tpidr);

    static NativeExecutionParameters* RestoreGuestContext(void* raw_context);
    static void SaveGuestContext(GuestContext* ctx, void* raw_context);

public:
    Core::System& m_system;

    // Members set on initialization.
    std::size_t m_core_index{};
    thread_id m_thread_id{NULL_THREAD_ID};

    // Core context.
    GuestContext m_guest_ctx{};
    Kernel::KThread* m_running_thread{};

    // Stack for signal processing.
    std::unique_ptr<u8[]> m_stack{};
};

} // namespace Core
