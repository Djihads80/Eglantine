// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

TEST_CASE("Ampere ordinary gathers reject unproved half precision modes",
          "[video_core][ampere][release_guards]") {
    for (const u32 precision : {1U, 2U, 3U}) {
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x06027b63, 0x33c04407, 0x041e0f01U | (precision << 15), 0x000fc200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native texture gather mode");
        REQUIRE(lowered.error_offset == 0);
        REQUIRE(lowered.instructions.empty());
        REQUIRE(lowered.source_instruction_offsets.empty());
    }
}
