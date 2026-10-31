// SPDX-License-Identifier: 0BSD

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/tests/A64/testenv.h"

TEST_CASE("A64: Windows code cache commits only emitted capacity", "[a64][code_commit]") {
    A64TestEnv env;
    env.code_mem = {0xd2800540, 0x14000000};
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &env;
    config.code_cache_size = 512 * 1024 * 1024;

    PROCESS_MEMORY_COUNTERS_EX before{};
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),
                                sizeof(before)));
    Dynarmic::A64::Jit jit{config};
    PROCESS_MEMORY_COUNTERS_EX after{};
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)));
    CAPTURE(before.PrivateUsage, after.PrivateUsage);
    REQUIRE(after.PrivateUsage < before.PrivateUsage + 96 * 1024 * 1024);

    for (size_t run = 0; run < 3; ++run) {
        jit.ClearCache();
        jit.SetPC(0);
        jit.SetRegister(0, 0);
        env.ticks_left = 2;
        jit.Run();
        REQUIRE(jit.GetRegister(0) == 42);
        REQUIRE(jit.GetPC() == 4);
    }
}
