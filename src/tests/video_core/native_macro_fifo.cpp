// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"

namespace {
std::array<u32, 3> FifoInstruction(u32 method, bool end) {
    std::array<u32, 3> bits{};
    const auto field = [&](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, end);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(15 + lane * 36, 5, 24);
        field(20 + lane * 36, 5, 24);
        field(25 + lane * 36, 5, 24);
    }
    field(30, 16, method);
    field(82, 3, 5);
    field(85, 4, 3);
    return {bits[2], bits[1], bits[0]};
}
} // namespace

TEST_CASE("Owned native macro FIFOs preserve loads across DMA appends and repeated calls",
          "[video_core][native_macro]") {
    std::vector<u32> code;
    for (const auto instruction : {FifoInstruction(0x158, false), FifoInstruction(0x200, true),
                                   FifoInstruction(0x201, false)}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    Tegra::TuringMacroJit jit;
    jit.Prepare(code, 1);
    std::array<u32, 0xc00> ram{};
    std::vector<u32> parameters;
    std::vector<u32> reference;
    for (u32 invocation = 0; invocation < 1000; ++invocation) {
        const u32 count = 2 + invocation % 127;
        parameters.push_back(count);
        reference.assign(parameters.begin(), parameters.end());
        const auto run = [&](std::vector<u32>& input) {
            std::vector<std::pair<u32, u32>> writes;
            const auto result = jit.Execute(
                0, input, ram, [](u32) { return 0U; },
                [&](u32 method, u32 value) {
                    writes.emplace_back(method, value);
                    if (method == 0x158) {
                        for (u32 word = 0; word < value; ++word) {
                            input.push_back(invocation * 256 + word);
                        }
                    }
                    return true;
                });
            CHECK(result.success);
            CHECK(result.consumed == 3);
            return writes;
        };
        const auto expected = run(reference);
        const auto actual = run(parameters);
        REQUIRE(actual.size() == 3);
        CHECK(actual == expected);
        CHECK(parameters == reference);
        CHECK(actual[1] == std::pair<u32, u32>{0x200, invocation * 256});
        CHECK(actual[2] == std::pair<u32, u32>{0x201, invocation * 256 + 1});
        parameters.clear();
    }
}
#endif
