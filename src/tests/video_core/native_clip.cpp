// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/native_clip.h"

namespace {
using Shader::Ampere::Instruction;
Instruction Op(u32 opcode, u32 dest, u32 a, u32 b, u32 controls, u32 predicate = 7) {
    return {{opcode | (predicate << 12) | (dest << 16) | (a << 24), b, controls, 0}};
}
std::vector<Instruction> Exports() {
    // Synthetic read-only vertex shader with renamed position registers R12..15.
    std::vector<Instruction> code{Op(0x321, 0, 255, (0x2fc << 8) | 255, 0),
                                  Op(0x9c3, 4, 0, 0, 0x1e00),
                                  Op(0x9c3, 5, 0, 0, 0x1f00),
                                  Op(0x811, 0, 0, 0x10000, 0x0f8e28ff),
                                  Op(0xb82, 14, 0, 0x200, 0x8a00),
                                  Op(0xb82, 12, 0, 0, 0x8a00),
                                  Op(0xc20, 1, 15, 4, 0x08410000),
                                  Op(0xc20, 3, 15, 5, 0x08410000)};
    for (u32 pred = 0; pred < 7; ++pred) {
        const u32 a = pred == 6 ? 15 : pred < 4 ? 1 : 3;
        const u32 b = pred == 6 ? 255 : (12 + pred / 2) | ((pred & 1) ? 0x80000000U : 0);
        code.push_back(Op(0x20b, 0, a, b, 0x03f19000 | (pred << 17)));
    }
    code.push_back(Op(0x80d, 0, 0, 0xc4321111, 0x03fe0000));
    code.push_back(Op(0x80d, 0, 0, 0x80000000, 0x03f06000));
    code.push_back(Op(0x94d, 0, 0, 0, 0x03800000, 0));
    code.push_back(Op(0xb82, 0, 0, 0x400, 0x8a00));
    code.push_back(Op(0x322, 0, 255, 12 | (0x70 << 8), 0xcff));
    code.push_back(Op(0x322, 0, 255, (0x80 << 8), 0x4ff));
    code.push_back(Op(0x94d, 0, 0, 0, 0x03800000));
    return code;
}
} // namespace

TEST_CASE("Deferred clipping preserves read-only vertex exports", "[native-clip]") {
    const auto code = Exports();
    REQUIRE(Shader::Ampere::PassthroughFrustumExports(code));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
    const auto lowered = Shader::Ampere::LowerToMaxwell(code, {.allow_rasterizer_clip = true});
    REQUIRE(lowered);
    REQUIRE(lowered.requires_rasterizer_clip);
    // Both genuine ASTs remain; only the redundant early-rejection region disappears.
    for (u32 i : {19U, 20U}) {
        const auto word = lowered.instructions[lowered.source_instruction_offsets[i]];
        REQUIRE((word >> 48 & 0xfff8) == 0xeff0);
    }
    auto changed = code;
    changed[18].words[0] |= 12U << 16; // Clobbering positions makes the proof invalid.
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
    changed = code;
    changed[9].words[1] ^= 0x80000000U;
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
    changed = code;
    changed[20].words[1] = 0x7000;
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
}

TEST_CASE("Rasterizer clipping eligibility reacts to live pipeline changes", "[native-clip]") {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    Regs regs{};
    regs.viewport_clip_control.geometry_clip.Assign(Regs::ViewportClipControl::GeometryClip::FrustumXYZ);
    REQUIRE(VideoCommon::RasterizerFrustumClipCompatible(regs));
    for (u32 stage : {0U, 2U, 3U, 4U}) {
        regs.pipelines[stage].enable.Assign(1);
        REQUIRE_FALSE(VideoCommon::RasterizerFrustumClipCompatible(regs));
        regs.pipelines[stage].enable.Assign(0);
        REQUIRE(VideoCommon::RasterizerFrustumClipCompatible(regs));
    }
    regs.transform_feedback_enabled = 1;
    REQUIRE_FALSE(VideoCommon::RasterizerFrustumClipCompatible(regs));
    regs.transform_feedback_enabled = 0;
    regs.viewport_clip_control.geometry_clip.Assign(Regs::ViewportClipControl::GeometryClip::WZero);
    REQUIRE_FALSE(VideoCommon::RasterizerFrustumClipCompatible(regs));
    regs.viewport_clip_control.geometry_clip.Assign(Regs::ViewportClipControl::GeometryClip::FrustumXYZ);
    REQUIRE(VideoCommon::RasterizerFrustumClipCompatible(regs));
}

TEST_CASE("Clip proof permits independent work but rejects changed positions and predicate users", "[native-clip]") {
    auto code = Exports();
    code.insert(code.begin() + 16, Op(0x802, 24, 0, 123, 0xf00));
    code.insert(code.begin() + 18, Op(0x802, 25, 0, 456, 0xf00));
    REQUIRE(Shader::Ampere::PassthroughFrustumExports(code));
    REQUIRE(Shader::Ampere::LowerToMaxwell(code, {.allow_rasterizer_clip = true}));
    auto changed = code;
    changed[18] = Op(0x802, 15, 0, 1, 0xf00); // Position W changes after comparison.
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
    changed = code;
    changed[18] = Op(0x802, 25, 0, 456, 0xf00, 0); // Observes the queried predicate.
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
    changed = code;
    changed[18] = Op(0x947, 0, 0, 0, 0x03800000); // Control-flow proof is deliberately unsupported.
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(changed));
}

TEST_CASE("Clip proof refuses shader storage effects and carry predicate consumers", "[native-clip]") {
    auto code = Exports();
    code.insert(code.begin() + 18, Op(0x810, 24, 24, 1, 0x007fe4ff));
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(code));
    code = Exports();
    code.insert(code.begin() + 3, Op(0x986, 0, 0, 0, 0));
    REQUIRE_FALSE(Shader::Ampere::PassthroughFrustumExports(code));
}
