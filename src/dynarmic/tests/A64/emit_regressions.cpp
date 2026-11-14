// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Emitter regressions found by tools/jitbench/fuzz: each instruction compiled in a normal block
// (Run) must give the result single-stepping gives, without the emitter throwing.

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "./testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class HaltOnSvcEnv final : public A64TestEnv {
public:
    A64::Jit* jit = nullptr;
    bool svc = false;
    void CallSVC(std::uint32_t) override {
        svc = true;
        jit->HaltExecution();
    }
};

template<typename Gen>
Vector RunV0(bool step, Gen&& gen) {
    HaltOnSvcEnv env;
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    gen(code);
    code.SVC(0);
    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(0);
    jit.SetVector(0, {0x0123456789ABCDEF, 0xFEDCBA9876543210});
    jit.SetVector(1, {0x1122334455667788, 0x99AABBCCDDEEFF00});
    if (step) {
        while (!env.svc) jit.Step();
    } else {
        jit.Run();
    }
    return jit.GetVector(0);
}

}  // namespace

TEST_CASE("A64: SM4E table address", "[a64][emit_regressions]") {
    // The S-box was addressed as an absolute disp32 memory operand: Xbyak error 15 ("can't use
    // 64bit disp") whenever the host image sits above 4 GiB (PIE, Windows).
    const auto gen = [](auto& code) { code.SM4E(V0.S4(), V1.S4()); };
    REQUIRE(RunV0(false, gen) == RunV0(true, gen));
}
