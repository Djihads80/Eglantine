// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/macro_dma_forward.h"
#include "video_core/macro_turing.h"

namespace {
// Ocarina of Time's macro 0x16 (start 0) from a trace: the first 40 instructions, each
// {word 2, word 1, word 0}. It reads 6 words of an occlusion report and writes the first to
// render_enable.mode (0x556).
constexpr std::array<std::array<u32, 3>, 40> Macro16{{
    {0x31c10300, 0x18c00455, 0x00940003},
    {0x318c0300, 0x18c01856, 0x8a600003},
    {0x359c1f00, 0x18c00400, 0x00000007},
    {0x31c08300, 0x1d000000, 0x00000003},
    {0x3bcc0300, 0x18c00000, 0x00000003},
    {0x3810bf00, 0xa003c000, 0x00000003},
    {0x0010bb00, 0x18c00000, 0x00000003},
    {0x72213c29, 0x18c00000, 0x0018155b},
    {0x302c0300, 0x18c00000, 0x00200003},
    {0x31c00300, 0x18c00538, 0x0d145387},
    {0x018c4300, 0x18c40021, 0x20200230},
    {0x018c4300, 0x18c40022, 0x20200200},
    {0xb18c0300, 0x18c40559, 0x00540250},
    {0x018c4301, 0x18c40027, 0x20200260},
    {0x018c4300, 0x18c00024, 0x00200003},
    {0xf1907b00, 0x1c08014e, 0x00000003},
    {0x00105701, 0x18c00000, 0x00000003},
    {0xb01c0002, 0x39c7c0dd, 0x003414ec},
    {0xf18c0300, 0x18c00121, 0x00140003},
    {0x318c0300, 0x18c003eb, 0x0141a207},
    {0xb18c0300, 0x18c00153, 0x3d144803},
    {0x31d08300, 0x18c00000, 0x40600003},
    {0x31c00300, 0x1d100000, 0x00600003},
    {0x058c4300, 0x58c40046, 0x20200c00},
    {0x718c0300, 0x18c00000, 0x60a00003},
    {0x718c0300, 0x38c404ff, 0x00550800},
    {0x018c4300, 0x18c01080, 0x9c201537},
    {0x318c0300, 0x18c0015d, 0x9d144757},
    {0x318c0300, 0x18c000e0, 0x0d144c03},
    {0xf18c0300, 0x18c03fff, 0xa1400003},
    {0x318c0300, 0x18c00000, 0x00a00003},
    {0x318c0300, 0x18c00fe0, 0x01400003},
    {0xf18c0300, 0x18c00192, 0x0d14193f},
    {0x318c0300, 0x18c00307, 0x9ca05947},
    {0xf18c0300, 0x18c00437, 0x9d140d6b},
    {0xf18c0300, 0x18c0012c, 0x9d1419bf},
    {0x318c0300, 0x18c00138, 0x9d1412ef},
    {0x318c0300, 0x18c00136, 0x9d1419c7},
    {0x318c0300, 0x18c001a0, 0x00d44447},
    {0x318c0300, 0x18c00021, 0x9d14153f},
}};

std::vector<u32> Code() {
    std::vector<u32> code;
    for (const auto& instruction : Macro16) {
        code.push_back(instruction[2]);
        code.push_back(instruction[1]);
        code.push_back(instruction[0]);
    }
    return code;
}

constexpr u32 MacroRegistersStart = 0xe00;
constexpr u32 RenderEnableMode = 0x556;
} // namespace

TEST_CASE("Macro 0x16 forwards its render_enable.mode write", "[macro_dma_forward]") {
    const std::vector<u32> code = Code();
    const std::array<u32, 3> parameters{0x0, 0x21, 0x13960e20};
    std::array<u32, 0xc00> ram{};
    const auto plan = VideoCommon::DmaForward::Analyze(
        code, 0, parameters, ram, MacroRegistersStart,
        [](u32, bool& unknown) { unknown = false; return 0U; },
        [](u32 method) { return method == RenderEnableMode; });
    INFO(plan.reason);
    REQUIRE(plan.eligible);
    REQUIRE(plan.num_writes == 4);
    CHECK(plan.writes[0].method == 0x154);
    CHECK(plan.writes[0].value == 0x21);
    CHECK(plan.writes[1].method == 0x155);
    CHECK(plan.writes[1].value == 0x13960e20);
    CHECK(plan.writes[2].method == 0x158);
    CHECK(plan.writes[2].value == 6);
    CHECK_FALSE(plan.writes[2].forwarded);
    CHECK(plan.writes[3].method == RenderEnableMode);
    CHECK(plan.writes[3].forwarded);
    CHECK(plan.dma_write_index == 2);
    CHECK(plan.snapshot.first_forwarded == 3);
    CHECK(plan.snapshot.dma_source == ((GPUVAddr{0x21} << 32) | 0x13960e20));
    CHECK(plan.snapshot.dma_words == 6);
    CHECK(plan.num_ram_writes == 0);
}

TEST_CASE("DMA forwarding refuses a method that may not wait for the back", "[macro_dma_forward]") {
    const std::vector<u32> code = Code();
    const std::array<u32, 3> parameters{0x0, 0x21, 0x13960e20};
    std::array<u32, 0xc00> ram{};
    const auto plan = VideoCommon::DmaForward::Analyze(
        code, 0, parameters, ram, MacroRegistersStart,
        [](u32, bool& unknown) { unknown = false; return 0U; }, [](u32) { return false; });
    CHECK_FALSE(plan.eligible);
    CHECK_FALSE(plan.reason.empty());
}

TEST_CASE("DMA forwarding refuses a call that runs out of parameters", "[macro_dma_forward]") {
    const std::vector<u32> code = Code();
    const std::array<u32, 1> parameters{0x0};
    std::array<u32, 0xc00> ram{};
    const auto plan = VideoCommon::DmaForward::Analyze(
        code, 0, parameters, ram, MacroRegistersStart,
        [](u32, bool& unknown) { unknown = false; return 0U; },
        [](u32 method) { return method == RenderEnableMode; });
    CHECK_FALSE(plan.eligible);
}

TEST_CASE("Recompute gives what macro 0x16's real run writes", "[macro_dma_forward]") {
    const std::vector<u32> code = Code();
    std::array<u32, 0xc00> ram{};
    // Counter pairs that differ, that are equal and zero, and equal and non-zero (where copying
    // word 0 would be wrong), with both senses of parameter 0.
    const std::array<std::array<u32, 6>, 4> reports{{{1, 0, 0, 0, 0, 0},
                                                     {0, 0, 0x5dbd248e, 0x17, 0, 0},
                                                     {5, 0, 0x5dbd248e, 0x17, 5, 0},
                                                     {5, 1, 0, 0, 5, 2}}};
    for (const u32 sense : {0U, 1U}) {
        for (const auto& report : reports) {
            CAPTURE(sense, report[0], report[1], report[4], report[5]);
            const std::array<u32, 3> parameters{sense, 0x21, 0x13960e00};
            const auto plan = VideoCommon::DmaForward::Analyze(
                code, 0, parameters, ram, MacroRegistersStart,
                [](u32, bool& unknown) { unknown = false; return 0U; },
                [](u32 method) { return method == RenderEnableMode; });
            INFO(plan.reason);
            REQUIRE(plan.eligible);
            // The interpreter with the real words, as the drained front path runs it.
            Tegra::TuringMacroInterpreter interpreter;
            std::vector<u32> fifo(parameters.begin(), parameters.end());
            std::vector<std::pair<u32, u32>> writes;
            const auto result = interpreter.Execute(
                code, 0, fifo, [](u32) { return 0U; },
                [&](u32 method, u32 value) {
                    writes.emplace_back(method, value);
                    if (method == 0x158) {
                        fifo.insert(fifo.end(), report.begin(), report.begin() + value);
                    }
                    return method < MacroRegistersStart;
                });
            INFO(result.error);
            REQUIRE(result.success);
            REQUIRE(writes.size() == plan.num_writes);
            const u32 expected = sense ^ ((report[0] != report[4] || report[1] != report[5]) ? 1U : 0U);
            CHECK(writes.back() == std::pair<u32, u32>{RenderEnableMode, expected});
            for (size_t i = 0; i < plan.snapshot.first_forwarded; ++i) {
                CHECK(writes[i] == std::pair<u32, u32>{plan.writes[i].method, plan.writes[i].value});
            }
            // The back's recomputation from the snapshot.
            std::vector<std::pair<u32, u32>> forwarded;
            REQUIRE(VideoCommon::DmaForward::Recompute(
                plan.snapshot, report, MacroRegistersStart,
                [&](u32 method, u32 value) { forwarded.emplace_back(method, value); }));
            REQUIRE(forwarded.size() == 1);
            CHECK(forwarded[0] == std::pair<u32, u32>{RenderEnableMode, expected});
        }
    }
}
