// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;

// Every texture reads as an integer format, as an out-of-range TIC entry did in Ocarina of Time.
class IntegerTextureEnvironment final : public Environment {
public:
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 2);
        REQUIRE(offset == 0x520);
        return 0x1234;
    }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::R16G16_UINT;
    }
    bool IsTexturePixelFormatInteger(u32) override { return true; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};
} // Anonymous namespace

// OpImageSampleDref* needs a float sampled type: an integer one is invalid SPIR-V, and RADV's
// spirv_to_nir failed on it and crashed the pipeline build.
TEST_CASE("Depth-compare texture descriptors stay float on an integer texture format",
          "[video_core][texture]") {
    IntegerTextureEnvironment env;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    IR::IREmitter ir{block};
    const IR::Value coords{ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f))};
    IR::TextureInstInfo info{};
    info.type.Assign(TextureType::Color2D);
    info.is_depth.Assign(1);
    (void)ir.ImageSampleDrefImplicitLod(ir.Imm32(0x520U), coords, ir.Imm32(0.5f), {}, {}, {},
                                        info);
    info.is_depth.Assign(0);
    (void)ir.ImageSampleImplicitLod(ir.Imm32(0x520U), coords, {}, {}, {}, info);

    HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Optimization::TexturePass(env, program, host_info);
    const auto& descriptors = program.info.texture_descriptors;
    REQUIRE(descriptors.size() == 2);
    for (const auto& descriptor : descriptors) {
        REQUIRE(descriptor.is_integer == !descriptor.is_depth);
    }
}
