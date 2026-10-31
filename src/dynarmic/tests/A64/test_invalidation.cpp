// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem_start_address = 100;
    env.code_mem.clear();
    env.code_mem.emplace_back(0xd2800d80);  // MOV X0, 108
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements 2", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem.emplace_back(0xd2800100);  // MOV X0, 8
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}

namespace {
constexpr u32 MovX(u32 reg, u32 imm) {
    return 0xd2800000 | (imm << 5) | reg;
}

constexpr u32 BranchBy(s32 offset) {
    return 0x14000000 | (u32(offset / 4) & 0x3ffffff);
}
}  // namespace

TEST_CASE("invalidating a block reached through more than two linked branches relinks every branch", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::BlockLinking));

    env.code_mem.emplace_back(MovX(0, 1));
    env.code_mem.emplace_back(BranchBy(28));
    env.code_mem.emplace_back(MovX(0, 2));
    env.code_mem.emplace_back(BranchBy(20));
    env.code_mem.emplace_back(MovX(0, 3));
    env.code_mem.emplace_back(BranchBy(12));
    env.code_mem.emplace_back(MovX(0, 4));
    env.code_mem.emplace_back(BranchBy(4));
    env.code_mem.emplace_back(MovX(2, 42));
    env.code_mem.emplace_back(BranchBy(0));

    const auto run_from = [&](u64 pc) {
        jit.SetPC(pc);
        jit.SetRegister(0, 0);
        jit.SetRegister(2, 0);
        env.ticks_left = 4;
        CheckedRun([&]() { jit.Run(); });
    };

    for (int pass = 0; pass < 2; ++pass) {
        for (u64 entry = 0; entry < 4; ++entry) {
            run_from(entry * 8);
            REQUIRE(jit.GetRegister(0) == entry + 1);
            REQUIRE(jit.GetRegister(2) == 42);
        }
    }

    env.code_mem[8] = MovX(2, 69);
    jit.InvalidateCacheRange(32, 4);

    for (u64 entry = 0; entry < 4; ++entry) {
        run_from(entry * 8);
        REQUIRE(jit.GetRegister(0) == entry + 1);
        REQUIRE(jit.GetRegister(2) == 69);
    }
}

TEST_CASE("invalidating a range drops exactly the blocks that overlap it", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    env.code_mem.emplace_back(MovX(0, 1));
    env.code_mem.emplace_back(BranchBy(0));
    env.code_mem.emplace_back(MovX(0, 2));
    env.code_mem.emplace_back(BranchBy(0));
    env.code_mem.emplace_back(MovX(0, 3));
    env.code_mem.emplace_back(BranchBy(0));
    env.code_mem.emplace_back(MovX(0, 4));
    env.code_mem.emplace_back(BranchBy(0));

    const auto run_from = [&](u64 pc) {
        jit.SetPC(pc);
        jit.SetRegister(0, 0);
        env.ticks_left = 2;
        CheckedRun([&]() { jit.Run(); });
        return jit.GetRegister(0);
    };

    for (u64 entry = 0; entry < 4; ++entry) {
        REQUIRE(run_from(entry * 8) == entry + 1);
    }

    env.code_mem[0] = MovX(0, 10);
    env.code_mem[2] = MovX(0, 20);
    env.code_mem[4] = MovX(0, 30);
    env.code_mem[6] = MovX(0, 40);
    jit.InvalidateCacheRange(12, 8);

    REQUIRE(run_from(0) == 1);
    REQUIRE(run_from(8) == 20);
    REQUIRE(run_from(16) == 30);
    REQUIRE(run_from(24) == 4);
}
