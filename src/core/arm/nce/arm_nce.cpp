// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ARCHITECTURE_arm64
#include "common/dynamic_library.h"

// Certain functions have to be marked naked so that the compiler doesn't touch the stack
// or implement a return (we "artificially" return later by setting PC to the LR value)
#if defined(__clang__) || defined(__GNUC__)
#define YUZU_NAKED                                          \
        _Pragma("GCC diagnostic push")                      \
        _Pragma("GCC diagnostic ignored \"-Wreturn-type\"") \
        __attribute__((naked)) __attribute__((noipa))
#elif defined(_MSC_VER)
#define YUZU_NAKED __declspec(naked)
#else
#error Unsupported compiler
#endif

#if defined(__clang__) || defined(__GNUC__)
#define YUZU_NAKED_END _Pragma("GCC diagnostic pop")
#else
#define YUZU_NAKED_END
#endif

#include <memory>

#include "common/signal_chain.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/arm/nce/patcher.h"
#include "core/core.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#else
#include "common/dynamic_library.h"
#endif

namespace Core {

namespace {

#ifndef _WIN32
struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;
#endif

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;

} // namespace

YUZU_ALWAYS_INLINE
NativeExecutionParameters* ArmNce::GetGuestParameters() {
    NativeExecutionParameters* nep = nullptr;
#if defined(__APPLE__)
    nep = static_cast<NativeExecutionParameters*>(pthread_getspecific(ContextKey));
#elif defined(_WIN32)
    nep = static_cast<NativeExecutionParameters*>(TlsGetValue(ContextKey));
#elif defined(__linux__)
    asm volatile(
        "mrs %0, TPIDR_EL0\n"
        : "=r"(nep));
#endif
    return nep;
}

YUZU_ALWAYS_INLINE
void ArmNce::LockThreadParameters(NativeExecutionParameters* nep) {
    u32 value;
    do {
        do {
            value = nep->lock.load(std::memory_order_acquire);
        } while (value == SpinLockLocked);
    } while (!nep->lock.compare_exchange_weak(value, SpinLockLocked,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed));
}

YUZU_ALWAYS_INLINE
void ArmNce::UnlockThreadParameters(NativeExecutionParameters* nep) {
    nep->lock.store(SpinLockUnlocked, std::memory_order_release);
}

#ifndef _WIN32
YUZU_NAKED
YUZU_NO_INLINE
HaltReason ArmNce::ReturnToRunCodeByExceptionLevelChange(thread_id tid, NativeExecutionParameters* tpidr) {
    // x0  - tid
    // x1  - tpidr

    // x9 - NativeExecutionParameters* (on Linux)
    //
    // uses tkill on linux and pthread_kill on macOS, both have the same signature:
    /* syscall (u32 tid, u64 signal) */
    // tid is already in x0 so we don't have to explicitly pass it

    asm volatile(
#if defined(__linux__)
        "mov x9, x1\n" // move tpidr to x9 so it doesn't get clobbered

        "mov x1, #%[sig]\n"  // SIGUSR2
        "mov x8, %[syscall]\n"
        "svc #0\n"
        "brk 0x0\n"
        :: [syscall] "i"(__NR_tkill),
#elif defined(__APPLE__)
        "mov x1, #%[sig]\n"  // SIGUSR2
        "mov x16, #328\n"
        "svc #0x80\n"
        "brk 0x0\n"
        ::
#else
        "brk 0x0\n"
        ::
#endif
        [sig] "i"(SIGUSR2)
        : "memory"
        );
}
YUZU_NAKED_END
#else
HaltReason ArmNce::ReturnToRunCodeByExceptionLevelChange(thread_id tid, NativeExecutionParameters *tpidr) {
    RaiseException(ExceptionLevelChangeSignal, 0, 0, nullptr); // TODO: pass tpidr through arguments?
    __builtin_unreachable();
}
#endif

void ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void *info, void *raw_context) {
    NativeExecutionParameters* nep = RestoreGuestContext(raw_context);

#if !defined(__APPLE__) && !defined(_WIN32)
    // Save old value of TPIDR_EL0, load guest one
    void* tpidr;
    asm volatile("mrs %0, TPIDR_EL0\n"
                 "msr TPIDR_EL0, %1\n"
                 : "=r"(tpidr)
                 : "r"(nep));
    nep->native_context->host_ctx.host_tpidr_el0 = tpidr;
#else
    nep->is_actually_running = true;
#endif

    UnlockThreadParameters(nep);
    // sigaction restores context and returns to guest
}

YUZU_NAKED
YUZU_NO_INLINE
HaltReason ArmNce::ReturnToRunCodeByTrampoline(NativeExecutionParameters* nep, u64 trampoline_addr) {
    // x0 - NativeExecutionParameters*
    // x1 - trampoline_addr

    // x2 - GuestContext*
    // x3 - Host SP
    // x4 - Host TPIDR_EL0 on non-Apple, is_actually_running on Apple
    // x5 - Scratch register

    asm volatile(
        "mov x3, SP\n"
        "ldr x2, [ x0, #%[ctx_off] ]\n"
        "add x5, x2, #%[host_ctx] \n"

#ifdef __linux__
        // Load guest tpidr_el0
        "mrs x4, TPIDR_EL0\n"
        "msr TPIDR_EL0, x0\n"
        // Store host sp and tpidr_el0
        "stp x3, x4, [x5, #0xE0]\n"
#else
        // is_actually_running = true
        "mov w4, #1\n"
        "strb w0, [ x0, #%[is_running_off] ]\n"
        // Store host sp
        "str x3, [x5, #0xE0]\n"
#endif

        // Save callee-saved host GPR registers
        "stp     x19, x20, [x5, #0x0]\n"
        "stp     x21, x22, [x5, #0x10]\n"
        "stp     x23, x24, [x5, #0x20]\n"
        "stp     x25, x26, [x5, #0x30]\n"
        "stp     x27, x28, [x5, #0x40]\n"
        "stp     x29, x30, [x5, #0x50]\n"

        // Save callee-saved host vector registers
        "stp     q8, q9,   [x5, #(0x60)]\n"
        "stp     q10, q11, [x5, #(0x80)]\n"
        "stp     q12, q13, [x5, #(0xA0)]\n"
        "stp     q14, q15, [x5, #(0xC0)]\n"

        "ldr x5, [ x2, #%[sp_off] ]\n"
        "mov SP, x5\n"

        "br x1\n"
        "brk 0x0\n"

        :: [ctx_off] "i"(offsetof(NativeExecutionParameters, native_context)),
        [sp_off] "i"(offsetof(GuestContext, sp)),
        [host_ctx] "i"(offsetof(GuestContext, host_ctx))
#if defined(__APPLE__) || defined(_WIN32)
        ,[is_running_off] "i"(offsetof(NativeExecutionParameters, is_actually_running))
#endif
        );
}
YUZU_NAKED_END

static_assert(offsetof(HostContext, host_sp) == 0xE0); // TODO: don't use magic number
static_assert(offsetof(HostContext, host_tpidr_el0) - 0xE0 == 8);

#ifndef _WIN32
void ArmNce::BreakFromRunCodeSignalHandler(int sig, void *info, void *raw_context) {
    NativeExecutionParameters* tpidr = GetGuestParameters();
#if defined(__APPLE__) || defined(_WIN32)
    if (tpidr->is_actually_running) {
        tpidr->is_actually_running = false;
#else
    if (tpidr->magic == Common::MakeMagic('Y', 'U', 'Z', 'U')) {
        // Load the host's TPIDR_EL0 value
        void* host_tpidr = tpidr->native_context->host_ctx.host_tpidr_el0;
        asm volatile(
            "msr TPIDR_EL0, %[host_tpidr]\n"
            :: [host_tpidr] "r"(host_tpidr));
#endif
        SaveGuestContext(tpidr->native_context, raw_context);
        // SaveGuestContext loads host context, returning from here will enter host code.
    }
}
#endif

#ifdef _WIN32

#define SIGSEGV EXCEPTION_ACCESS_VIOLATION
#define SIGBUS EXCEPTION_DATATYPE_MISALIGNMENT
using PFN_SetProcessValidCallTargets = BOOL(WINAPI*)(
    _In_ HANDLE hProcess, _In_ PVOID VirtualAddress, _In_ SIZE_T RegionSize,
    _In_ ULONG NumberOfOffsets, _In_ PCFG_CALL_TARGET_INFO OffsetInformation);

static PFN_SetProcessValidCallTargets pfn_SetProcessValidCallTargets {};

LONG WINAPI ArmNce::VectoredExceptionHandler(PEXCEPTION_POINTERS info) {
    // TODO: Windows doesn't allocate a separate stack so we're either
    // on the guest or current host stack, is that okay?
    DWORD code = info->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_DATATYPE_MISALIGNMENT) {
        NativeExecutionParameters* nep = GetGuestParameters();

        if (nep->is_actually_running) {
            nep->is_actually_running = GuestMemoryFaultSignalHandler(nep, code, info->ExceptionRecord->ExceptionAddress, info->ContextRecord);
            return EXCEPTION_CONTINUE_EXECUTION;
        } else {
            // host fault, pass it to the original handler
            return EXCEPTION_CONTINUE_SEARCH;
        }
    } else if (code == ExceptionLevelChangeSignal) {
        ReturnToRunCodeByExceptionLevelChangeSignalHandler(code, reinterpret_cast<void*>(&info->ExceptionRecord->ExceptionAddress), info->ContextRecord);

        // TODO: is this the right way to do this?
        CFG_CALL_TARGET_INFO targetInfo = {0};
        targetInfo.Offset = 0;
        targetInfo.Flags = CFG_CALL_TARGET_VALID;

        if (!pfn_SetProcessValidCallTargets) {
            // TODO: move this to Initialize
            Common::DynamicLibrary kernelbase {"Kernelbase"};
            ASSERT(kernelbase.GetSymbol("SetProcessValidCallTargets", &pfn_SetProcessValidCallTargets));
        }
        pfn_SetProcessValidCallTargets(GetCurrentProcess(), reinterpret_cast<void*>(info->ContextRecord->Pc), 0x1000, 1, &targetInfo);
        return EXCEPTION_CONTINUE_EXECUTION;
    } else {
        // other exception? let it pass
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
#else
void ArmNce::POSIXExceptionHandler(int sig, void* raw_info, void* raw_context) {
    NativeExecutionParameters* nep = GetGuestParameters();

#ifndef __linux__
    if (!nep->is_actually_running) {
#else
    if (nep->magic != Common::MakeMagic('Y', 'U', 'Z', 'U')) {
#endif
        // Host fault, call original handler
        // TODO: add fallback for null sigactions
        if (sig == SIGSEGV) {
            g_orig_segv_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
        } else if (sig == SIGBUS) {
            g_orig_bus_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
        } else [[unlikely]] {
            UNREACHABLE_MSG("unexpected signal {}", sig);
        }
    }

#ifdef __linux__
    // Load the host's TPIDR_EL0 value
    void* host_tpidr = nep->native_context->host_ctx.host_tpidr_el0;
    asm volatile(
        "msr TPIDR_EL0, %[host_tpidr]\n"
        :: [host_tpidr] "r"(host_tpidr));

    if (GuestMemoryFaultSignalHandler(nep, sig, reinterpret_cast<u64>(static_cast<siginfo_t*>(raw_info)->si_addr), raw_context)) {
        // Returning to guest, restore guest's TPIDR_EL0
        asm volatile(
            "msr TPIDR_EL0, %0\n"
            :: "r"(nep));
    }
#else
    nep->is_actually_running = GuestMemoryFaultSignalHandler(nep, sig, static_cast<siginfo_t*>(raw_info)->si_addr, raw_context);
#endif
}
#endif

bool ArmNce::GuestMemoryFaultSignalHandler(NativeExecutionParameters* nep, int sig, u64 fault_addr, void* raw_context) {
    auto* guest_ctx = nep->native_context;
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();

    LOG_DEBUG(Core_ARM, "Signal handler called on address {:#x}", fault_addr);

    if (sig == SIGSEGV) {
        // Try to handle an invalid access.
        // TODO: handle accesses which split a page?
        const Common::ProcessAddress addr = fault_addr & ~(Common::HostPageSize - 1);

        auto ret = memory.InvalidateNCE(addr, Common::HostPageSize);
        if (ret == 0) {
            // We handled the access successfully and are returning to guest code.
            return true;
        } else if (ret == 1) {
            // This access is in an unmapped region, we need to use the interpreter.
        } else if (ret == -1) {
            // We couldn't handle the access.
            return HandleFailedGuestFault(guest_ctx, fault_addr, raw_context);
        }
    }

    // Match and execute an instruction.
    auto ctx = KernelContext(raw_context);
    auto next_pc = MatchAndExecuteOneInstruction(memory, ctx);
    if (next_pc) {
        // We handled the access successfully and are returning to guest code.
        *ctx.pc() = *next_pc;
        return true;
    } else {
        // We couldn't handle the access.
        return HandleFailedGuestFault(guest_ctx, fault_addr, raw_context);
    }
}

NativeExecutionParameters* ArmNce::RestoreGuestContext(void* raw_context) {
    // Retrieve the host context.
    auto host_ctx = KernelContext(raw_context);

#ifdef __linux__
    // Thread-local parameters will be located in x9.
    auto* nep = reinterpret_cast<NativeExecutionParameters*>(host_ctx.regs()[9]);
#else
    auto* nep = GetGuestParameters();
#endif

    auto* guest_ctx = nep->native_context;

    // Save host callee-saved registers.
    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &host_ctx.vregs()[8],
                sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs()[19],
                sizeof(guest_ctx->host_ctx.host_saved_regs));

    // Save stack pointer.
    guest_ctx->host_ctx.host_sp = *host_ctx.sp();

    // Restore all guest state except tpidr_el0.
    *host_ctx.sp() = guest_ctx->sp;
    *host_ctx.pc() = guest_ctx->pc;
    *host_ctx.pstate() = guest_ctx->pstate;
    *host_ctx.fpcr() = guest_ctx->fpcr;
    *host_ctx.fpsr() = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs(), guest_ctx->cpu_registers.data(), sizeof(guest_ctx->cpu_registers));
    std::memcpy(host_ctx.vregs(), guest_ctx->vector_registers.data(), sizeof(guest_ctx->vector_registers));

#ifdef _WIN32
    auto tib = (PNT_TIB)NtCurrentTeb();
    // Save host stack base/limit
    nep->host_stack_base = tib->StackBase;
    nep->host_stack_limit = tib->StackLimit;
    // Set guest stack base/limit for CET
    tib->StackBase = nep->guest_stack_base;
    tib->StackLimit = nep->guest_stack_limit;
#endif

    // Return the new thread-local storage pointer.
    return nep;
}

void ArmNce::SaveGuestContext(GuestContext* guest_ctx, void* raw_context) {
    // Retrieve the host context.
    auto host_ctx = KernelContext(raw_context);

    // Save all guest registers except tpidr_el0.
    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs(), sizeof(guest_ctx->cpu_registers));
    std::memcpy(guest_ctx->vector_registers.data(), host_ctx.vregs(), sizeof(guest_ctx->vector_registers));
    guest_ctx->fpsr = *host_ctx.fpsr();
    guest_ctx->fpcr = *host_ctx.fpcr();
    guest_ctx->pstate = *host_ctx.pstate();
    guest_ctx->pc = *host_ctx.pc();
    guest_ctx->sp = *host_ctx.sp();

    // Restore stack pointer.
    *host_ctx.sp() = guest_ctx->host_ctx.host_sp;

#ifdef _WIN32
    // Restore stack base/limit for CET
    // TODO: store this in Context instead of NEP?
    auto tib = (PNT_TIB)NtCurrentTeb();
    tib->StackBase = GetGuestParameters()->host_stack_base;
    tib->StackLimit = GetGuestParameters()->host_stack_limit;
#endif

    // Restore host callee-saved registers.
    std::memcpy(&host_ctx.regs()[19], guest_ctx->host_ctx.host_saved_regs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&host_ctx.vregs()[8], guest_ctx->host_ctx.host_saved_vregs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_vregs));

    // Return from the call on exit by setting pc to x30.
    *host_ctx.pc() = guest_ctx->host_ctx.host_saved_regs[11];

    // Clear esr_el1 and return it.
    host_ctx.regs()[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleFailedGuestFault(GuestContext* guest_ctx, u64 raw_info, void* raw_context) {
    auto host_ctx = KernelContext(raw_context);

    // We can't handle the access, so determine why we crashed.
    const bool is_prefetch_abort = *host_ctx.pc() == raw_info;

    // For data aborts, skip the instruction and return to guest code.
    // This will allow games to continue in many scenarios where they would otherwise crash.
    if (!is_prefetch_abort) {
        *host_ctx.pc() += 4;
        return true;
    }

    // This is a prefetch abort.
    guest_ctx->esr_el1.fetch_or(static_cast<u64>(HaltReason::PrefetchAbort));

    // Forcibly mark the context as locked. We are still running.
    // We may race with SignalInterrupt here:
    // - If we lose the race, then SignalInterrupt will send us a signal we are masking,
    //   and it will do nothing when it is unmasked, as we have already left guest code.
    // - If we win the race, then SignalInterrupt will wait for us to unlock first.
    auto& thread_params = guest_ctx->parent->m_running_thread->GetNativeExecutionParameters();
    thread_params.lock.store(SpinLockLocked);

    // Return to host.
    SaveGuestContext(guest_ctx, raw_context);
    return false;
}

void ArmNce::LockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(thread_params);
}

void ArmNce::UnlockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    m_guest_ctx.tpidrro_el0 = thread_params->tpidrro_el0;
    thread_params->native_context = nullptr;
    UnlockThreadParameters(thread_params);
}

HaltReason ArmNce::RunThread(Kernel::KThread* thread) {
    // Check if we're already interrupted.
    // If we are, we can just return immediately.
    HaltReason hr = static_cast<HaltReason>(m_guest_ctx.esr_el1.exchange(0));
    if (True(hr)) {
        return hr;
    }

    // Pre-fetch thread context data to improve cache locality
    auto* thread_params = &thread->GetNativeExecutionParameters();
    auto* process = thread->GetOwnerProcess();

#if defined(__APPLE__)
    ASSERT(pthread_setspecific(ContextKey, thread_params) == 0);
#elif defined(_WIN32)
    ASSERT_MSG(TlsSetValue(ContextKey, thread_params), "Failed to set TLS value: id {}, error {}", ContextKey, GetLastError());
    ASSERT_MSG(TlsSetValue(NCEStorage, reinterpret_cast<void*>(m_guest_ctx.cpu_registers[18])),
        "Failed to set TLS value: id {}, error {}", ContextKey, GetLastError());

    thread_params->guest_stack_base = reinterpret_cast<void*>(GetInteger(
        thread->GetOwnerProcess()->GetPageTable().GetStackRegionStart() + thread->GetOwnerProcess()->GetPageTable().GetStackRegionSize()));
    thread_params->guest_stack_limit = reinterpret_cast<void*>(GetInteger(thread->GetOwnerProcess()->GetPageTable().GetStackRegionStart()));

    ClearInstructionCache();
#endif

    // Move non-critical operations outside the locked section
    const u64 tpidr_el0_cache = m_guest_ctx.tpidr_el0;
    const u64 tpidrro_el0_cache = m_guest_ctx.tpidrro_el0;

    // Critical section begins - minimize operations here
    m_running_thread = thread;
    m_guest_ctx.parent = this;
    thread_params->native_context = &m_guest_ctx;
    thread_params->tpidr_el0 = tpidr_el0_cache;
    thread_params->tpidrro_el0 = tpidrro_el0_cache;

    // Memory barrier to ensure visibility of changes
    std::atomic_thread_fence(std::memory_order_release);
    thread_params->is_running = true;

    // TODO: finding and creating the post handler needs to be locked
    // to deal with dynamic loading of NROs.
    const auto& post_handlers = process->GetPostHandlers();
    if (auto it = post_handlers.find(m_guest_ctx.pc); it != post_handlers.end()) {
        hr = ReturnToRunCodeByTrampoline(thread_params, it->second);
    } else {
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);  // Android: Use "process handle SIGUSR2 -n true -p true -s false" (and SIGURG) in LLDB when debugging
    }

    // Critical section for thread cleanup
    std::atomic_thread_fence(std::memory_order_acquire);

    // Cache values before releasing thread
    const u64 final_tpidr_el0 = thread_params->tpidr_el0;

    // Minimize critical section
    thread_params->is_running = false;
    thread_params->native_context = nullptr;
    m_running_thread = nullptr;

    // Non-critical updates can happen after releasing the thread
    m_guest_ctx.tpidr_el0 = final_tpidr_el0;

    // Return the halt reason.
    return hr;
}

HaltReason ArmNce::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

u32 ArmNce::GetSvcNumber() const {
    return m_guest_ctx.svc;
}

void ArmNce::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_guest_ctx.cpu_registers[i];
    }
}

void ArmNce::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; i++) {
        m_guest_ctx.cpu_registers[i] = args[i];
    }
}

ArmNce::ArmNce(System& system, bool uses_wall_clock, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_core_index{core_index} {
    m_guest_ctx.system = &m_system;
}

ArmNce::~ArmNce() = default;

#ifdef __APPLE__
// https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/src/pthread_tsd.c#L418-L435
// Equivalent to pthread_key_create but allows for setting a specific key value.
// Used in internal WebKit and certain Apple programs to define and use a reserved key.
extern "C" int pthread_key_init_np(int, void (*)(void *));
#endif

void ArmNce::Initialize() {

    if (m_thread_id == NULL_THREAD_ID) {

#if defined(__APPLE__)
        m_thread_id = pthread_mach_thread_np(pthread_self());
        ASSERT(pthread_key_init_np(ContextKey, [](void*) -> void {}) == 0);
#elif defined(__linux)
        m_thread_id = gettid();
#elif defined(_WIN32)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
            &m_thread_id, 0, false, DUPLICATE_SAME_ACCESS);
#endif

    }

#ifndef _WIN32
    // Configure signal stack.
    if (!m_stack) {
        m_stack = std::make_unique<u8[]>(StackSize);

        stack_t ss{};
        ss.ss_sp = m_stack.get();
        ss.ss_size = StackSize;
        sigaltstack(&ss, nullptr);
    }

    // Set up signals.
    static std::once_flag flag;
    std::call_once(flag, [] {
        using HandlerType = decltype(sigaction::sa_sigaction);

        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGUSR2); // ReturnToCodeByExceptionLevel
        sigaddset(&signal_mask, SIGURG);  // BreakFromRunCode
        sigaddset(&signal_mask, SIGBUS);
        sigaddset(&signal_mask, SIGSEGV);

        struct sigaction return_to_run_code_action {};
        return_to_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        return_to_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(
            &ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
        return_to_run_code_action.sa_mask = signal_mask;
        Common::SigAction(SIGUSR2, &return_to_run_code_action,
                          nullptr);

        struct sigaction break_from_run_code_action {};
        break_from_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        break_from_run_code_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
        break_from_run_code_action.sa_mask = signal_mask;
        Common::SigAction(SIGURG, &break_from_run_code_action, nullptr);

        struct sigaction alignment_fault_action {};
        alignment_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        alignment_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::POSIXExceptionHandler);
        alignment_fault_action.sa_mask = signal_mask;
        Common::SigAction(SIGBUS, &alignment_fault_action, nullptr);

        struct sigaction access_fault_action {};
        access_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        access_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::POSIXExceptionHandler);
        access_fault_action.sa_mask = signal_mask;
        Common::SigAction(SIGSEGV, &access_fault_action, &g_orig_segv_action);
    });
#else
    static std::once_flag flag;
    std::call_once(flag, [] { AddVectoredExceptionHandler(1, VectoredExceptionHandler); });
#endif
}

void ArmNce::SetTpidrroEl0(u64 value) {
    m_guest_ctx.tpidrro_el0 = value;
}

void ArmNce::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = m_guest_ctx.cpu_registers[i];
    }
    ctx.fp = m_guest_ctx.cpu_registers[29];
    ctx.lr = m_guest_ctx.cpu_registers[30];
    ctx.sp = m_guest_ctx.sp;
    ctx.pc = m_guest_ctx.pc;
    ctx.pstate = m_guest_ctx.pstate;
    ctx.v = m_guest_ctx.vector_registers;
    ctx.fpcr = m_guest_ctx.fpcr;
    ctx.fpsr = m_guest_ctx.fpsr;
    ctx.tpidr = m_guest_ctx.tpidr_el0;
}

void ArmNce::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; i++) {
        m_guest_ctx.cpu_registers[i] = ctx.r[i];
    }
    m_guest_ctx.cpu_registers[29] = ctx.fp;
    m_guest_ctx.cpu_registers[30] = ctx.lr;
    m_guest_ctx.sp = ctx.sp;
    m_guest_ctx.pc = ctx.pc;
    m_guest_ctx.pstate = ctx.pstate;
    m_guest_ctx.vector_registers = ctx.v;
    m_guest_ctx.fpcr = ctx.fpcr;
    m_guest_ctx.fpsr = ctx.fpsr;
    m_guest_ctx.tpidr_el0 = ctx.tpidr;
}

void ArmNce::SignalInterrupt(Kernel::KThread* thread) {
    // Add break loop condition.
    m_guest_ctx.esr_el1.fetch_or(static_cast<u64>(HaltReason::BreakLoop));

    auto* params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    // Ensure visibility of is_running after lock acquire
    std::atomic_thread_fence(std::memory_order_acquire);

    if (params->is_running) {
        // We should signal to the running thread.
        // The running thread will unlock the thread context.
#if defined(__linux__)
        syscall(SYS_tkill, m_thread_id, SIGURG); // BreakFromRunCodeSignal
#elif defined(__APPLE__)
        asm volatile(
            "mov x0, %0\n"    // m_thread_id
            "mov x1, %1\n"    // BreakFromRunCodeSignal
            "mov x16, #328\n" // syscall code for __pthread_kill
            "svc #0x80\n"
            :: "r"(static_cast<u64>(m_thread_id)), "r"(static_cast<u64>(SIGURG))
            : "x0", "x1", "x16", "memory", "cc");
#elif defined(_WIN32)
        // TODO: use Get/SetThreadState to emulate BreakFromRunCodeSignalHandler
        SuspendThread(m_thread_id);
        UnlockThreadParameters(params);
#endif
    } else {
        // If the thread is no longer running, we have nothing to do.
        UnlockThreadParameters(params);
    }
}

void ArmNce::ClearInstructionCache() {
#ifndef _WIN32
    // Ensure all previous memory operations complete
    asm volatile("dsb ish\n"
                 "isb" ::: "memory");
#else
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
#endif
}

void ArmNce::InvalidateCacheRange(u64 addr, std::size_t size) {
#ifndef _WIN32
    ClearInstructionCache();
#else
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
#endif
}

} // namespace Core

#endif // #ifdef ARCHITECTURE_arm64
