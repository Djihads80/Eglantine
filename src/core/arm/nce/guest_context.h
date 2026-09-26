// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/arm/arm_interface.h"

#ifdef __linux__
#include <signal.h>
#elif _WIN32
#include <windows.h>
#undef interface
#endif

namespace Core {

class ArmNce;
class System;

struct HostContext {
    alignas(16) std::array<u64, 12> host_saved_regs{};
    alignas(16) std::array<u128, 8> host_saved_vregs{};
    u64 host_sp{};
    void* host_tpidr_el0{};
};

struct GuestContext {
    std::array<u64, 31> cpu_registers{};
    u64 sp{};
    u64 pc{};
    u32 fpcr{};
    u32 fpsr{};
    std::array<u128, 32> vector_registers{};
    u32 pstate{};
    alignas(16) HostContext host_ctx{};
    u64 tpidrro_el0{};
    u64 tpidr_el0{};
    std::atomic<u64> esr_el1{};
    u32 nzcv{};
    u32 svc{};
    System* system{};
    ArmNce* parent{};
};

class KernelContext {
public:
#if defined(__linux__)
    explicit KernelContext(void* ptr_) : ptr(&static_cast<ucontext_t *>(ptr_)->uc_mcontext), fpsimd{GetFloatingPointState(ptr)} {}

    u64* pc() {
        // u64 (unsigned long) does not equal unsigned long long
        // thank you gcc
        return reinterpret_cast<u64*>(&ptr->pc);
    }

    u64* sp() {
        return reinterpret_cast<u64*>(&ptr->sp);
    }

    u64* regs() {
        return reinterpret_cast<u64*>(&ptr->regs);
    }

    u128* vregs() {
        // returns __uint128, u128 is a std::array
        return reinterpret_cast<u128*>(&fpsimd->vregs);
    }

    u32* fpcr() {
        return &fpsimd->fpcr;
    }

    u32* fpsr() {
        return &fpsimd->fpsr;
    }

    u32* pstate() {
        // only first 32 bits are used
        return reinterpret_cast<u32*>(&ptr->pstate);
    }

#elif defined(__APPLE__)
    explicit KernelContext(void* ptr) : ptr(static_cast<ucontext_t*>(ptr)->uc_mcontext) {}

    u64* pc() {
        return &ptr->__ss.__pc;
    }

    u64* sp() {
        return &ptr->__ss.__sp;
    }

    u64* regs() {
        return ptr->__ss.__x;
    }

    u128* vregs() {
        // .__v returns __uint128, u128 is an std::array
        return reinterpret_cast<u128 *>(ptr->__ns.__v);
    }

    u32* fpcr() {
        return &ptr->__ns.__fpcr;
    }

    u32* fpsr() {
        return &ptr->__ns.__fpsr;
    }

    u32* pstate() {
        return &ptr->__ss.__cpsr;
    }
#elif defined(_WIN32)
    explicit KernelContext(void* ptr) : ptr(static_cast<CONTEXT*>(ptr)) {}

    u64* pc() {
        return &ptr->Pc;
    }

    u64* sp() {
        return &ptr->Sp;
    }

    u64* regs() {
        return ptr->X;
    }

    u128* vregs() {
        // V returns ARM64_NT_NEON128, u128 is an std::array
        return reinterpret_cast<u128 *>(ptr->V);
    }

    u32* fpcr() {
        // unsigned long vs unsigned int
        return reinterpret_cast<u32*>(&ptr->Fpcr);
    }

    u32* fpsr() {
        // unsigned long vs unsigned int
        return reinterpret_cast<u32*>(&ptr->Fpsr);
    }

    u32* pstate() {
        // unsigned long vs unsigned int
        return reinterpret_cast<u32*>(&ptr->Cpsr);
    }
#endif

private:
#if defined(__APPLE__)
    mcontext_t ptr;
#elif defined(__linux__)
    mcontext_t* ptr;
    fpsimd_context* fpsimd;

    fpsimd_context* GetFloatingPointState(mcontext_t* host_ctx) {
        _aarch64_ctx* header = reinterpret_cast<_aarch64_ctx*>(&host_ctx->__reserved);
        while (header->magic != FPSIMD_MAGIC) {
            header = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<char*>(header) + header->size);
        }
        return reinterpret_cast<fpsimd_context*>(header);
    }
#elif defined(_WIN32)
    CONTEXT* ptr;
#endif
};

#ifdef __APPLE__
// ensure that fp and lr are next to the rest of the x registers so they can be accessed like an array
static_assert(offsetof(_STRUCT_ARM_THREAD_STATE64, __sp) - offsetof(_STRUCT_ARM_THREAD_STATE64, __x) == sizeof(u64) * 31);
#endif

} // namespace Core
