// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/signal_chain.h"
#include "core/arm/debug.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/arm/nce/patcher.h"
#include "core/arm/nce_stats.h"
#include "core/core.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace Core {

namespace {

struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;

// Verify assembly offsets.
using NativeExecutionParameters = Kernel::KThread::NativeExecutionParameters;
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);
static_assert(offsetof(NativeExecutionParameters, magic) == TpidrEl0TlsMagic);

fpsimd_context* GetFloatingPointState(mcontext_t& host_ctx) {
    _aarch64_ctx* header = reinterpret_cast<_aarch64_ctx*>(&host_ctx.__reserved);
    while (header->magic != FPSIMD_MAGIC) {
        header = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<char*>(header) + header->size);
    }
    return reinterpret_cast<fpsimd_context*>(header);
}

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;

// ---- Diagnostics: fatal guest signals, repeating faults and core sampling ----

// Reads (guest = host, under NCE) memory without faulting.
bool SafeRead(u64 address, void* out, size_t size) {
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

// The four instruction words at PC-8, PC-4, [PC] and PC+4; ???????? where unreadable.
std::string CodeAround(u64 pc) {
    std::string text;
    for (u32 i = 0; i < 4; ++i) {
        const u64 address = pc - 8 + i * 4;
        u32 word{};
        const bool readable = SafeRead(address, &word, sizeof(word));
        if (!text.empty()) text += ' ';
        if (address == pc) text += '[';
        text += readable ? fmt::format("{:08x}", word) : std::string{"????????"};
        if (address == pc) text += ']';
    }
    return text;
}

// A record in the signal frame's __reserved area (FPSIMD, ESR, ...), or null.
const _aarch64_ctx* FindSigcontextRecord(const mcontext_t& host_ctx, u32 magic) {
    const auto* base = reinterpret_cast<const u8*>(&host_ctx.__reserved);
    size_t offset = 0;
    while (offset + sizeof(_aarch64_ctx) <= sizeof(host_ctx.__reserved)) {
        const auto* header = reinterpret_cast<const _aarch64_ctx*>(base + offset);
        if (header->magic == 0 || header->size == 0) return nullptr;
        if (header->magic == magic) return header;
        offset += header->size;
    }
    return nullptr;
}

// ESR_EL1 of a synchronous fault, as the kernel reports it (0 if absent).
u64 FaultEsr(const mcontext_t& host_ctx) {
    const auto* record = FindSigcontextRecord(host_ctx, ESR_MAGIC);
    return record ? reinterpret_cast<const esr_context*>(record)->esr : 0;
}

const char* AccessKind(u64 esr) {
    const u64 ec = esr >> 26;
    if (ec == 0x20 || ec == 0x21) return "instruction fetch";
    if (ec == 0x24 || ec == 0x25) return (esr >> 6) & 1 ? "write" : "read";
    return "access";
}

// A data abort on a write (not a cache maintenance instruction) to a page without write
// permission: a store to a GPU-cached page.
bool IsWritePermissionFault(u64 esr) {
    const u64 ec = esr >> 26;
    const bool data_abort = ec == 0x24 || ec == 0x25;
    const bool write = ((esr >> 6) & 1) != 0;
    const bool cache_maintenance = ((esr >> 8) & 1) != 0;
    const u64 dfsc = esr & 0x3f;
    return data_abort && write && !cache_maintenance && dfsc >= 0x0c && dfsc <= 0x0f;
}

// A data abort on a read (not a cache maintenance instruction): a load from a page without read
// permission, such as an armed read trap. Without an ESR record (0) it is not taken as one.
bool IsReadFault(u64 esr) {
    const u64 ec = esr >> 26;
    const bool data_abort = ec == 0x24 || ec == 0x25;
    const bool write = ((esr >> 6) & 1) != 0;
    const bool cache_maintenance = ((esr >> 8) & 1) != 0;
    return data_abort && !write && !cache_maintenance;
}

// EDEN_NCE_EMULATE_WRITES=<n>: emulate up to n stores per GPU-cached page (in a row on a core)
// before invalidating it. EDEN_NCE_FAULT_AHEAD=<n>: after a write fault on the GPU-cached page
// following the previous one, invalidate the next n pages too. Read once, after env.txt.
std::atomic<u32> g_emulate_writes{};
std::atomic<u32> g_fault_ahead{};

u32 ReadSwitch(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0;
}

const char* SignalName(int sig) {
    return sig == SIGILL ? "SIGILL" : sig == SIGTRAP ? "SIGTRAP" : sig == SIGFPE ? "SIGFPE" : "signal";
}

struct sigaction g_orig_ill_action;
struct sigaction g_orig_trap_action;
struct sigaction g_orig_fpe_action;

// Hands a fatal signal on as if this handler were not installed.
void ChainFatalSignal(int sig, siginfo_t* info, void* raw_context) {
    const struct sigaction& original = sig == SIGILL    ? g_orig_ill_action
                                       : sig == SIGTRAP ? g_orig_trap_action
                                                        : g_orig_fpe_action;
    if ((original.sa_flags & SA_SIGINFO) != 0 && original.sa_sigaction != nullptr) {
        original.sa_sigaction(sig, info, raw_context);
        return;
    }
    if ((original.sa_flags & SA_SIGINFO) == 0 && original.sa_handler != SIG_DFL &&
        original.sa_handler != SIG_IGN) {
        original.sa_handler(sig);
        return;
    }
    // The default action: a fault re-executes into it on return, a sent signal is sent again.
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    Common::SigAction(sig, &default_action, nullptr);
    if (info->si_code <= 0) {
        syscall(SYS_tgkill, getpid(), gettid(), sig);
    }
}

void ReportGuestFatalSignal(int sig, const siginfo_t* info, void* raw_context,
                            const NativeExecutionParameters* params) {
    const auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    const auto* guest_ctx = static_cast<const GuestContext*>(params->native_context);
    const ArmNce* core = guest_ctx->parent;
    Kernel::KThread* const thread = core != nullptr ? core->m_running_thread : nullptr;
    const u64 pc = host_ctx.pc;
    LOG_CRITICAL(Core_ARM,
                 "NCE guest {} (si_code {}) at PC {:#x}, address {:#x}, core {}, host tid {}, "
                 "guest thread {}, guest TLS {:#x}, ESR {:#x}",
                 SignalName(sig), info->si_code, pc, reinterpret_cast<u64>(info->si_addr),
                 core != nullptr ? static_cast<s64>(core->m_core_index) : -1, gettid(),
                 thread != nullptr ? static_cast<s64>(thread->GetThreadId()) : -1,
                 params->tpidrro_el0, FaultEsr(host_ctx));
    LOG_CRITICAL(Core_ARM, "NCE guest code at {:#x}: {}", pc - 8, CodeAround(pc));
    for (u32 i = 0; i < 24; i += 8) {
        LOG_CRITICAL(Core_ARM,
                     "NCE x{:02}-x{:02}: {:016x} {:016x} {:016x} {:016x} {:016x} {:016x} {:016x} "
                     "{:016x}",
                     i, i + 7, host_ctx.regs[i], host_ctx.regs[i + 1], host_ctx.regs[i + 2],
                     host_ctx.regs[i + 3], host_ctx.regs[i + 4], host_ctx.regs[i + 5],
                     host_ctx.regs[i + 6], host_ctx.regs[i + 7]);
    }
    LOG_CRITICAL(Core_ARM,
                 "NCE x24-x30: {:016x} {:016x} {:016x} {:016x} {:016x} {:016x} {:016x} sp {:016x} "
                 "pstate {:08x}",
                 host_ctx.regs[24], host_ctx.regs[25], host_ctx.regs[26], host_ctx.regs[27],
                 host_ctx.regs[28], host_ctx.regs[29], host_ctx.regs[30], host_ctx.sp,
                 host_ctx.pstate);
    Common::Log::Stop(); // flushes every backend
    // Best effort: the guest modules and frame-pointer chain (reads guest memory, allocates).
    if (thread != nullptr && thread->GetOwnerProcess() != nullptr) {
        Kernel::Svc::ThreadContext ctx{};
        for (size_t i = 0; i < 29; ++i) {
            ctx.r[i] = host_ctx.regs[i];
        }
        ctx.fp = host_ctx.regs[29];
        ctx.lr = host_ctx.regs[30];
        ctx.sp = host_ctx.sp;
        ctx.pc = host_ctx.pc;
        ctx.pstate = static_cast<u32>(host_ctx.pstate);
        for (const auto& entry : GetBacktraceFromContext(thread->GetOwnerProcess(), ctx)) {
            LOG_CRITICAL(Core_ARM, "NCE backtrace: {} +{:#x} (address {:#x}) {}", entry.module,
                         entry.offset, entry.original_address, entry.name);
        }
        Common::Log::Stop();
    }
}

// SIGILL, SIGTRAP and SIGFPE. Guest code runs with the guest's tpidr_el0 (whose TLS magic
// identifies it), so the host's is restored before anything touches libc; this entry itself must
// not read the stack guard from TLS.
__attribute__((no_stack_protector)) void GuestFatalSignalHandler(int sig, siginfo_t* info,
                                                                 void* raw_context) {
    u64 tpidr;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    const auto* params = reinterpret_cast<const NativeExecutionParameters*>(tpidr);
    if (params == nullptr || params->magic != TlsMagic || params->native_context == nullptr) {
        ChainFatalSignal(sig, info, raw_context); // host code: unchanged
        return;
    }
    const auto* guest_ctx = static_cast<const GuestContext*>(params->native_context);
    asm volatile("msr tpidr_el0, %0" : : "r"(guest_ctx->host_ctx.host_tpidr_el0) : "memory");
    ReportGuestFatalSignal(sig, info, raw_context, params);
    // Leave the host tpidr_el0 in place: whatever runs next for this signal is host code.
    ChainFatalSignal(sig, info, raw_context);
}

// A guest access fault that comes back this many times in a row at the same PC and address is
// reported (and again every RepeatedFaultReminder repeats while it lasts).
constexpr u64 RepeatedFaultThreshold = 1000;
constexpr u64 RepeatedFaultReminder = 1ULL << 20;

void NoteGuestAccessFault(ArmNce* core, const mcontext_t& host_ctx, const siginfo_t* info,
                          const Memory::Memory::NceInvalidateResult& invalidate, bool handled) {
    const u64 pc = host_ctx.pc, address = reinterpret_cast<u64>(info->si_addr);
    if (pc != core->m_repeat_fault_pc || address != core->m_repeat_fault_address) {
        core->m_repeat_fault_pc = pc;
        core->m_repeat_fault_address = address;
        core->m_repeat_fault_count = 1;
        return;
    }
    const u64 count = ++core->m_repeat_fault_count;
    if (count != RepeatedFaultThreshold && count % RepeatedFaultReminder != 0) {
        return;
    }
    const u64 esr = FaultEsr(host_ctx);
    u8 probe{};
    const bool readable = SafeRead(address, &probe, sizeof(probe));
    const char* lifted = invalidate.rasterizer             ? "GPU cache invalidate"
                         : invalidate.read_trap            ? "read trap taken"
                         : invalidate.separate_heap_mapped ? "separate heap mapped in"
                                                           : "nothing (protection unchanged)";
    Kernel::KThread* const thread = core->m_running_thread;
    LOG_ERROR(Core_ARM,
              "NCE guest fault repeated {} times in a row: {} of {:#x} (page offset {:#x}) at PC "
              "{:#x}, si_code {}, ESR {:#x} (DFSC {:#x}), page mapped {}, fix applied: {}, "
              "handled {}, readable now {}, core {}, guest thread {}",
              count, AccessKind(esr), address, address & Memory::YUZU_PAGEMASK, pc, info->si_code,
              esr, esr & 0x3f, invalidate.mapped, lifted, handled, readable, core->m_core_index,
              thread != nullptr ? static_cast<s64>(thread->GetThreadId()) : -1);
    LOG_ERROR(Core_ARM, "NCE guest code at {:#x}: {}", pc - 8, CodeAround(pc));
}

// EDEN_NX2_NCE_SAMPLE=1: every 2 s each core's thread is sent a signal whose handler records
// where it was (guest or host code), to show where a spinning core is.
struct NceSampleSlot {
    std::atomic<u64> sequence{};
    std::atomic<u64> pc{};
    std::atomic<u64> lr{};
    std::atomic<u64> sp{};
    std::atomic<u64> x0{};
    std::atomic<u64> guest_tls{};
    std::atomic<u32> in_guest{};
};
static_assert(std::atomic<u64>::is_always_lock_free);
NceSampleSlot g_sample;
std::atomic<u64> g_sample_wanted{};
int g_sample_signal = -1;
std::mutex g_sample_cores_mutex;
std::vector<ArmNce*> g_sample_cores;

// Touches no TLS: it may interrupt guest code, which runs with the guest's tpidr_el0.
__attribute__((no_stack_protector)) void NceSampleSignalHandler(int, siginfo_t*,
                                                                void* raw_context) {
    const auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    u64 tpidr;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    const auto* params = reinterpret_cast<const NativeExecutionParameters*>(tpidr);
    const bool in_guest = params != nullptr && params->magic == TlsMagic;
    g_sample.pc.store(host_ctx.pc, std::memory_order_relaxed);
    g_sample.lr.store(host_ctx.regs[30], std::memory_order_relaxed);
    g_sample.sp.store(host_ctx.sp, std::memory_order_relaxed);
    g_sample.x0.store(host_ctx.regs[0], std::memory_order_relaxed);
    g_sample.guest_tls.store(in_guest ? params->tpidrro_el0 : 0, std::memory_order_relaxed);
    g_sample.in_guest.store(in_guest ? 1 : 0, std::memory_order_relaxed);
    g_sample.sequence.store(g_sample_wanted.load(std::memory_order_relaxed),
                            std::memory_order_release);
}

void NceSamplerLoop() {
    using namespace std::chrono_literals;
    for (u64 round = 1;; ++round) {
        std::this_thread::sleep_for(2s);
        std::scoped_lock lk{g_sample_cores_mutex};
        for (ArmNce* core : g_sample_cores) {
            const pid_t tid = core->m_thread_id;
            if (tid <= 0) continue;
            const u64 wanted = g_sample_wanted.fetch_add(1) + 1;
            if (syscall(SYS_tgkill, getpid(), tid, g_sample_signal) != 0) continue;
            bool sampled = false;
            for (u32 wait = 0; wait < 50 && !sampled; ++wait) {
                sampled = g_sample.sequence.load(std::memory_order_acquire) == wanted;
                if (!sampled) std::this_thread::sleep_for(1ms);
            }
            const u64 last_exit_pc = core->m_guest_ctx.pc;
            const u32 last_svc = core->m_guest_ctx.svc;
            if (!sampled) {
                LOG_WARNING(Core_ARM,
                            "NCE sample {}: core {} (tid {}) did not answer; last exit PC {:#x}, "
                            "last SVC {:#x}",
                            round, core->m_core_index, tid, last_exit_pc, last_svc);
                continue;
            }
            const u64 pc = g_sample.pc.load(std::memory_order_relaxed);
            const bool in_guest = g_sample.in_guest.load(std::memory_order_relaxed) != 0;
            LOG_WARNING(Core_ARM,
                        "NCE sample {}: core {} (tid {}) in {} code, PC {:#x} LR {:#x} SP {:#x} X0 "
                        "{:#x}, guest TLS {:#x}, code {}; last exit PC {:#x}, last SVC {:#x}",
                        round, core->m_core_index, tid, in_guest ? "guest" : "host", pc,
                        g_sample.lr.load(std::memory_order_relaxed),
                        g_sample.sp.load(std::memory_order_relaxed),
                        g_sample.x0.load(std::memory_order_relaxed),
                        g_sample.guest_tls.load(std::memory_order_relaxed),
                        in_guest ? CodeAround(pc) : std::string{"-"}, last_exit_pc, last_svc);
        }
    }
}

} // namespace

void* ArmNce::RestoreGuestContext(void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Thread-local parameters will be located in x9.
    auto* tpidr = reinterpret_cast<NativeExecutionParameters*>(host_ctx.regs[9]);
    auto* guest_ctx = static_cast<GuestContext*>(tpidr->native_context);

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save host callee-saved registers.
    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &fpctx->vregs[8],
                sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs[19],
                sizeof(guest_ctx->host_ctx.host_saved_regs));

    // Save stack pointer.
    guest_ctx->host_ctx.host_sp = host_ctx.sp;

    // Restore all guest state except tpidr_el0.
    host_ctx.sp = guest_ctx->sp;
    host_ctx.pc = guest_ctx->pc;
    host_ctx.pstate = guest_ctx->pstate;
    fpctx->fpcr = guest_ctx->fpcr;
    fpctx->fpsr = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs, guest_ctx->cpu_registers.data(), sizeof(host_ctx.regs));
    std::memcpy(fpctx->vregs, guest_ctx->vector_registers.data(), sizeof(fpctx->vregs));

    // Return the new thread-local storage pointer.
    return tpidr;
}

void ArmNce::SaveGuestContext(GuestContext* guest_ctx, void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save all guest registers except tpidr_el0.
    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs, sizeof(host_ctx.regs));
    std::memcpy(guest_ctx->vector_registers.data(), fpctx->vregs, sizeof(fpctx->vregs));
    guest_ctx->fpsr = fpctx->fpsr;
    guest_ctx->fpcr = fpctx->fpcr;
    guest_ctx->pstate = static_cast<u32>(host_ctx.pstate);
    guest_ctx->pc = host_ctx.pc;
    guest_ctx->sp = host_ctx.sp;

    // Restore stack pointer.
    host_ctx.sp = guest_ctx->host_ctx.host_sp;

    // Restore host callee-saved registers.
    std::memcpy(&host_ctx.regs[19], guest_ctx->host_ctx.host_saved_regs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&fpctx->vregs[8], guest_ctx->host_ctx.host_saved_vregs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_vregs));

    // Return from the call on exit by setting pc to x30.
    host_ctx.pc = guest_ctx->host_ctx.host_saved_regs[11];

    // Clear esr_el1 and return it.
    host_ctx.regs[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleFailedGuestFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* info = static_cast<siginfo_t*>(raw_info);

    // We can't handle the access, so determine why we crashed.
    const bool is_prefetch_abort = host_ctx.pc == reinterpret_cast<u64>(info->si_addr);

    // For data aborts, skip the instruction and return to guest code.
    // This will allow games to continue in many scenarios where they would otherwise crash.
    if (!is_prefetch_abort) {
        // Diagnostics: the first 32 skips, then every 4096th.
        static std::atomic<u64> skipped{};
        const u64 count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32 || count % 4096 == 0) {
            const u64 esr = FaultEsr(host_ctx);
            LOG_ERROR(Core_ARM,
                      "NCE skipped unhandled guest data abort #{} (signal {}): {} of {:#x} at PC "
                      "{:#x}, si_code {}, ESR {:#x}, core {}, code {}",
                      count, info->si_signo, AccessKind(esr), reinterpret_cast<u64>(info->si_addr),
                      host_ctx.pc, info->si_code, esr, guest_ctx->parent->m_core_index,
                      CodeAround(host_ctx.pc));
        }
        host_ctx.pc += 4;
        return true;
    }

    // This is a prefetch abort.
    LOG_ERROR(Core_ARM, "NCE guest prefetch abort at PC {:#x}, si_code {}, core {}, LR {:#x}",
              host_ctx.pc, info->si_code, guest_ctx->parent->m_core_index, host_ctx.regs[30]);
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

bool ArmNce::HandleGuestAlignmentFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* fpctx = GetFloatingPointState(host_ctx);
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();
    const bool stats = NceStats::Enabled();
    const u64 start = stats ? NceStats::Ticks() : 0;
    SCOPE_EXIT {
        if (stats) {
            NceStats::RecordFault(guest_ctx->parent->m_core_index, NceStats::Fault::Alignment,
                                  NceStats::Ticks() - start);
        }
    };

    // Match and execute an instruction.
    auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx);
    if (next_pc) {
        host_ctx.pc = *next_pc;
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

bool ArmNce::HandleGuestAccessFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto* info = static_cast<siginfo_t*>(raw_info);
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    ArmNce* const core = guest_ctx->parent;
    const bool stats = NceStats::Enabled();
    const u64 start = stats ? NceStats::Ticks() : 0;

    // Try to handle an invalid access.
    // TODO: handle accesses which split a page?
    const Common::ProcessAddress addr =
        (reinterpret_cast<u64>(info->si_addr) & ~Memory::YUZU_PAGEMASK);
    auto& memory = core->m_running_thread->GetOwnerProcess()->GetMemory();

    const u64 esr = FaultEsr(host_ctx);

    // A store to a GPU-cached page.
    const u32 emulate_budget = g_emulate_writes.load(std::memory_order_relaxed);
    const u32 ahead = g_fault_ahead.load(std::memory_order_relaxed);
    bool sequential = false;
    if (stats || emulate_budget != 0 || ahead != 0) {
        if (IsWritePermissionFault(esr) && memory.IsRasterizerCachedPage(addr)) {
            const u64 page = GetInteger(addr);
            sequential = page == core->m_last_raster_page + Memory::YUZU_PAGESIZE;
            if (stats) {
                auto& counters = NceStats::Counters(core->m_core_index);
                if (sequential) {
                    NceStats::Add(counters.sequential_faults, 1);
                } else if (page == core->m_last_raster_page) {
                    NceStats::Add(counters.same_page_faults, 1);
                }
                if (((esr >> 24) & 1) != 0) {
                    NceStats::Add(counters.scalar_faults, 1);
                }
            }
            core->m_last_raster_page = page;
            if (emulate_budget != 0) {
                // As Dynarmic does: the store goes through Memory, whose GPU-cached path holds
                // it for the GPU thread to apply (GPU-written pages) or invalidates the caches,
                // without waiting for the GPU thread. The page stays write-protected while the
                // GPU caches it, so a page written more often than the budget falls back to
                // invalidating it below.
                if (core->TakeEmulateBudget(page, emulate_budget)) {
                    auto* fpctx = GetFloatingPointState(host_ctx);
                    if (const auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx)) {
                        host_ctx.pc = *next_pc;
                        if (stats) {
                            NceStats::RecordFault(core->m_core_index, NceStats::Fault::Emulated,
                                                  NceStats::Ticks() - start);
                        }
                        return true;
                    }
                }
                if (stats) {
                    NceStats::Add(NceStats::Counters(core->m_core_index).emulate_fallbacks, 1);
                }
            }
        }
    }

    if (stats) {
        NceStats::t_invalidate_waited = false;
    }
    Memory::Memory::NceInvalidateResult invalidate{};
    const bool handled =
        memory.InvalidateNCE(addr, Memory::YUZU_PAGESIZE, IsReadFault(esr), &invalidate);
    NoteGuestAccessFault(core, host_ctx, info, invalidate, handled);
    if (handled && !invalidate.rasterizer && !invalidate.read_trap &&
        !invalidate.separate_heap_mapped && core->m_repeat_fault_count % 16 == 0 &&
        memory.RepairNceReadTrapPage(addr)) {
        // Nothing lifted the fault 16 times in a row on a page a read trap protected once.
        LOG_WARNING(Core_ARM,
                    "NCE fault repeated {} times on a page that left the GPU caches with a read "
                    "trap's protection ({:#x}, PC {:#x}): made read-write",
                    core->m_repeat_fault_count, GetInteger(addr), host_ctx.pc);
    }
    if (handled) {
        if (invalidate.rasterizer && sequential && ahead != 0) {
            // A writer going through GPU-cached pages in order: take the next ones' faults now.
            const size_t pages = memory.InvalidateNCEAhead(addr + Memory::YUZU_PAGESIZE, ahead);
            core->m_last_raster_page += pages * Memory::YUZU_PAGESIZE;
            if (stats) {
                NceStats::Add(NceStats::Counters(core->m_core_index).ahead_pages, pages);
            }
        }
        if (stats) {
            const auto kind = invalidate.rasterizer
                                  ? (NceStats::t_invalidate_waited ? NceStats::Fault::RasterizerWait
                                                                   : NceStats::Fault::Rasterizer)
                              : invalidate.read_trap            ? NceStats::Fault::ReadTrap
                              : invalidate.separate_heap_mapped ? NceStats::Fault::SeparateHeap
                                                                : NceStats::Fault::Mapped;
            NceStats::RecordFault(core->m_core_index, kind, NceStats::Ticks() - start);
        }
        // We handled the access successfully and are returning to guest code.
        return true;
    }

    // We couldn't handle the access.
    const size_t core_index = core->m_core_index;
    const bool resume = HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
    if (stats) {
        NceStats::RecordFault(core_index, NceStats::Fault::Unhandled, NceStats::Ticks() - start);
    }
    return resume;
}

bool ArmNce::TakeEmulateBudget(u64 page, u32 budget) {
    for (EmulatedPage& entry : m_emulated_pages) {
        if (entry.page == page) {
            if (entry.stores >= budget) {
                entry.page = ~u64{};
                return false;
            }
            ++entry.stores;
            return true;
        }
    }
    m_emulated_pages[m_emulated_next++ % m_emulated_pages.size()] = {page, 1};
    return true;
}

bool ArmNce::HandleGuestIllegalInstruction(GuestContext* guest_ctx, void* raw_info,
                                           void* raw_context) {
    // knob2: skip undefined instructions in guest code, as Dynarmic does
    if (Settings::GetDebugKnobAt(2)) {
        static_cast<ucontext_t*>(raw_context)->uc_mcontext.pc += 4;
        return true;
    }

    // Registers, code and backtrace, as for the other fatal guest signals (GuestFatalSignalHandler).
    ReportGuestFatalSignal(SIGILL, static_cast<const siginfo_t*>(raw_info), raw_context,
                           &guest_ctx->parent->m_running_thread->GetNativeExecutionParameters());

    // Halt the guest thread instead of taking down the whole process.
    // The faulting pc equals si_addr, so this is handled as a prefetch abort.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

void ArmNce::HandleHostAlignmentFault(int sig, void* raw_info, void* raw_context) {
    return g_orig_bus_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
}

void ArmNce::HandleHostAccessFault(int sig, void* raw_info, void* raw_context) {
    return g_orig_segv_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
}

void ArmNce::HandleHostIllegalInstruction(int sig, void* raw_info, void* raw_context) {
    ChainFatalSignal(sig, static_cast<siginfo_t*>(raw_info), raw_context);
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
    const bool stats = NceStats::Enabled();
    const auto& post_handlers = process->GetPostHandlers();
    if (auto it = post_handlers.find(m_guest_ctx.pc); it != post_handlers.end()) {
        if (stats) {
            NceStats::Enter(m_core_index, NceStats::State::Guest);
            NceStats::Add(NceStats::Counters(m_core_index).runs, 1);
        }
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, it->second);
    } else {
        if (stats) {
            NceStats::Enter(m_core_index, NceStats::State::Guest);
            auto& counters = NceStats::Counters(m_core_index);
            NceStats::Add(counters.runs, 1);
            NceStats::Add(counters.signal_entries, 1);
        }
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);  // Android: Use "process handle SIGUSR2 -n true -p true -s false" (and SIGURG) in LLDB when debugging
    }
    if (stats) {
        NceStats::Enter(m_core_index, NceStats::State::Host);
        auto& counters = NceStats::Counters(m_core_index);
        if (True(hr & HaltReason::SupervisorCall)) {
            NceStats::Add(counters.svc_exits, 1);
        }
        if (True(hr & HaltReason::BreakLoop)) {
            NceStats::Add(counters.interrupt_exits, 1);
        }
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

ArmNce::~ArmNce() {
    std::scoped_lock lk{g_sample_cores_mutex};
    std::erase(g_sample_cores, this);
}

void ArmNce::Initialize() {
    const bool first_run = m_thread_id == -1;
    if (first_run) {
        m_thread_id = gettid();
    }

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
        sigaddset(&signal_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
        sigaddset(&signal_mask, BreakFromRunCodeSignal);
        sigaddset(&signal_mask, GuestAlignmentFaultSignal);
        sigaddset(&signal_mask, GuestAccessFaultSignal);
        sigaddset(&signal_mask, GuestIllegalInstructionSignal);

        struct sigaction return_to_run_code_action {};
        return_to_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        return_to_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(
            &ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
        return_to_run_code_action.sa_mask = signal_mask;
        Common::SigAction(ReturnToRunCodeByExceptionLevelChangeSignal, &return_to_run_code_action,
                          nullptr);

        struct sigaction break_from_run_code_action {};
        break_from_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        break_from_run_code_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
        break_from_run_code_action.sa_mask = signal_mask;
        Common::SigAction(BreakFromRunCodeSignal, &break_from_run_code_action, nullptr);

        struct sigaction alignment_fault_action {};
        alignment_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        alignment_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler);
        alignment_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAlignmentFaultSignal, &alignment_fault_action, nullptr);

        struct sigaction access_fault_action {};
        access_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        access_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler);
        access_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAccessFaultSignal, &access_fault_action, &g_orig_segv_action);

        // Diagnostics: report SIGTRAP/SIGFPE raised by guest code, then hand them on (SIGILL:
        // GuestIllegalInstructionSignalHandler below reports it and halts the guest thread).
        struct sigaction fatal_action {};
        fatal_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        fatal_action.sa_sigaction = &GuestFatalSignalHandler;
        // Only the run/interrupt signals are held off: a host fault while reporting still reaches
        // the SIGSEGV/SIGBUS handlers and the crash reporter behind them.
        sigemptyset(&fatal_action.sa_mask);
        sigaddset(&fatal_action.sa_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
        sigaddset(&fatal_action.sa_mask, BreakFromRunCodeSignal);
        Common::SigAction(SIGTRAP, &fatal_action, &g_orig_trap_action);
        Common::SigAction(SIGFPE, &fatal_action, &g_orig_fpe_action);

        struct sigaction illegal_instruction_action {};
        illegal_instruction_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        illegal_instruction_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestIllegalInstructionSignalHandler);
        // As for the other fatal signals, a host fault while reporting still reaches the
        // SIGSEGV/SIGBUS handlers (signal_mask would hold them off and the kernel kill us).
        illegal_instruction_action.sa_mask = fatal_action.sa_mask;
        Common::SigAction(GuestIllegalInstructionSignal, &illegal_instruction_action,
                          &g_orig_ill_action);

        g_emulate_writes.store(ReadSwitch("EDEN_NCE_EMULATE_WRITES"), std::memory_order_relaxed);
        g_fault_ahead.store(ReadSwitch("EDEN_NCE_FAULT_AHEAD"), std::memory_order_relaxed);
        if (g_emulate_writes.load() != 0 || g_fault_ahead.load() != 0) {
            LOG_INFO(Core_ARM,
                     "NCE GPU-cached page writes: {} stores emulated per page "
                     "(EDEN_NCE_EMULATE_WRITES), {} pages invalidated ahead (EDEN_NCE_FAULT_AHEAD)",
                     g_emulate_writes.load(), g_fault_ahead.load());
        }

        if (const char* sample = std::getenv("EDEN_NX2_NCE_SAMPLE");
            sample != nullptr && *sample != '\0' && *sample != '0') {
            g_sample_signal = SIGRTMAX - 1;
            struct sigaction sample_action {};
            sample_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
            sample_action.sa_sigaction = &NceSampleSignalHandler;
            sigemptyset(&sample_action.sa_mask);
            Common::SigAction(g_sample_signal, &sample_action, nullptr);
            std::thread{NceSamplerLoop}.detach();
            LOG_WARNING(Core_ARM, "NCE sampling every 2 s on signal {} (EDEN_NX2_NCE_SAMPLE)",
                        g_sample_signal);
        }
    });

    if (first_run && g_sample_signal >= 0) {
        std::scoped_lock lk{g_sample_cores_mutex};
        g_sample_cores.push_back(this);
    }
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
    constexpr u64 break_loop = static_cast<u64>(HaltReason::BreakLoop);
    if ((m_guest_ctx.esr_el1.fetch_or(break_loop) & break_loop) != 0) return;

    auto* params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    // Ensure visibility of is_running after lock acquire
    std::atomic_thread_fence(std::memory_order_acquire);

    if (params->is_running) {
        // We should signal to the running thread.
        // The running thread will unlock the thread context.
        syscall(SYS_tkill, m_thread_id, BreakFromRunCodeSignal);
        if (NceStats::Enabled() && m_core_index < NceStats::MaxCores) {
            NceStats::Counters(m_core_index)
                .interrupt_signals.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // If the thread is no longer running, we have nothing to do.
        UnlockThreadParameters(params);
    }
}

[[maybe_unused]] const std::size_t CACHE_PAGE_SIZE = 4096;

void ArmNce::ClearInstructionCache() {
#ifdef __aarch64__
    // Ensure all previous memory operations complete
    asm volatile("dsb ish\n"
                 "dsb ish\n"
                 "isb" ::: "memory");
#endif
}

void ArmNce::InvalidateCacheRange(u64 addr, std::size_t size) {
    this->ClearInstructionCache();
}

} // namespace Core
