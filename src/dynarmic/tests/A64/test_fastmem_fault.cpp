// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;

namespace {

constexpr u32 LdrX(u32 rt, u32 rn) {
    return 0xf9400000 | (rn << 5) | rt;
}

constexpr u32 StrX(u32 rt, u32 rn) {
    return 0xf9000000 | (rn << 5) | rt;
}

constexpr u32 branch_to_self = 0x14000000;
constexpr u32 address_space_bits = 16;
constexpr u64 load_address = 0x1238;
constexpr u64 store_address = 0x2000;
constexpr u64 loaded_value = 0x3f3e3d3c3b3a3938;
constexpr u64 faults_per_run = 2;

std::uintptr_t reserved_begin = 0;
std::uintptr_t reserved_end = 0;
thread_local u64 reserved_faults = 0;

LONG CALLBACK CountReservedFaults(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
        if (address >= reserved_begin && address < reserved_end) {
            ++reserved_faults;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

class FaultingFastmem {
public:
    FaultingFastmem()
            : base{VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS)}
            , handler{AddVectoredExceptionHandler(1, CountReservedFaults)} {
        REQUIRE(base != nullptr);
        REQUIRE(handler != nullptr);
        reserved_begin = Pointer();
        reserved_end = reserved_begin + size;
        reserved_faults = 0;
    }

    ~FaultingFastmem() {
        RemoveVectoredExceptionHandler(handler);
        VirtualFree(base, 0, MEM_RELEASE);
        reserved_begin = 0;
        reserved_end = 0;
    }

    FaultingFastmem(const FaultingFastmem&) = delete;
    FaultingFastmem& operator=(const FaultingFastmem&) = delete;

    std::uintptr_t Pointer() const {
        return reinterpret_cast<std::uintptr_t>(base);
    }

private:
    static constexpr std::size_t size = std::size_t(1) << address_space_bits;
    void* base;
    PVOID handler;
};

void LoadProgram(A64TestEnv& env) {
    env.code_mem.emplace_back(LdrX(1, 0));
    env.code_mem.emplace_back(StrX(1, 3));
    env.code_mem.emplace_back(branch_to_self);
}

A64::UserConfig FaultingConfig(A64TestEnv& env, const FaultingFastmem& memory, bool recompile) {
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.fastmem_pointer = memory.Pointer();
    conf.fastmem_address_space_bits = address_space_bits;
    conf.silently_mirror_fastmem = false;
    conf.recompile_on_fastmem_failure = recompile;
    return conf;
}

void RunLoadStore(A64TestEnv& env, A64::Jit& jit) {
    env.modified_memory.clear();
    jit.SetPC(0);
    jit.SetRegister(0, load_address);
    jit.SetRegister(1, 0);
    jit.SetRegister(3, store_address);
    env.ticks_left = 3;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetPC() == 8);
    REQUIRE(jit.GetRegister(1) == loaded_value);
    REQUIRE(env.MemoryRead(store_address, sizeof(u64)) == loaded_value);
}

}  // namespace

TEST_CASE("A64: fastmem faults resume through the fallbacks on every access", "[a64][fastmem]") {
    FaultingFastmem memory;
    A64TestEnv env;
    LoadProgram(env);
    A64::Jit jit{FaultingConfig(env, memory, false)};

    for (u64 run = 1; run <= 3; ++run) {
        RunLoadStore(env, jit);
        REQUIRE(reserved_faults == run * faults_per_run);
    }
}

TEST_CASE("A64: a fastmem fault recompiles its block to use the callbacks", "[a64][fastmem]") {
    FaultingFastmem memory;
    A64TestEnv env;
    LoadProgram(env);
    A64::Jit jit{FaultingConfig(env, memory, true)};

    for (u64 run = 1; run <= 3; ++run) {
        RunLoadStore(env, jit);
        REQUIRE(reserved_faults == faults_per_run);
    }
}
