// SPDX-License-Identifier: GPL-3.0-or-later

// Depth-compare sampling regressions from Lichii #273.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <unordered_map>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
class TextureEnvironment final : public Shader::Environment {
public:
    u32 binding{15};
    bool integer_format{false};
    std::span<const u64> code;

    TextureEnvironment() { stage = Shader::Stage::Compute; }

    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == 0x50 + index * 8 + ((index + 1) / 3) * 8) {
                return code[index];
            }
        }
        FAIL("Read outside logged texture fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override { return (bank << 16) | offset; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return integer_format;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return binding; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 4096; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

} // namespace

TEST_CASE("Depth sampling emits float image and result types for integer descriptors",
          "[video_core][spirv][depth_compare]") {
    for (const bool depth : {false, true}) {
        for (const bool integer : {false, true}) {
            CAPTURE(depth, integer);
            TextureEnvironment env;
            env.integer_format = integer;
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            IR::Program program;
            program.stage = Shader::Stage::Fragment;
            program.blocks = {&block};
            program.post_order_blocks = {&block};
            program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                                   {{}, IR::AbstractSyntaxNode::Type::Return}};
            IR::IREmitter ir{block};
            IR::TextureInstInfo flags{};
            flags.type.Assign(Shader::TextureType::Color2D);
            flags.is_depth.Assign(depth ? 1 : 0);
            const auto coords = ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f));
            const auto sample = depth
                ? IR::Value{ir.ImageSampleDrefImplicitLod(ir.Imm32(0), coords, ir.Imm32(0.5f),
                                                        {}, {}, {}, flags)}
                : ir.ImageSampleExplicitLod(ir.Imm32(0), coords, ir.Imm32(0.0f), {}, flags);
            Shader::HostTranslateInfo host;
            host.ApplyDescriptorLimitPolicy();
            Shader::Optimization::TexturePass(env, program, host);
            REQUIRE(program.info.texture_descriptors.size() == 1);
            CHECK(program.info.texture_descriptors[0].is_integer == (integer && !depth));
            CHECK(sample.Inst()->Flags<IR::TextureInstInfo>().is_integer ==
                  static_cast<u32>(integer && !depth));
            Shader::Profile profile;
            profile.supported_spirv = 0x00010500;
            const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            std::unordered_map<u32, u32> scalar_types;
            u32 image_sampled_type{};
            u32 dref_result_type{};
            for (size_t offset = 5; offset < code.size();) {
                const u32 size = code[offset] >> 16;
                REQUIRE(size != 0);
                REQUIRE(offset + size <= code.size());
                const u32 opcode = code[offset] & 0xffffU;
                if (opcode == 21 || opcode == 22) {
                    scalar_types.emplace(code[offset + 1], opcode);
                } else if (opcode == 25) {
                    image_sampled_type = code[offset + 2];
                    CHECK(code[offset + 4] == static_cast<u32>(depth));
                } else if (opcode == 89) {
                    dref_result_type = code[offset + 1];
                }
                offset += size;
            }
            REQUIRE(image_sampled_type != 0);
            CHECK(scalar_types.at(image_sampled_type) == (integer && !depth ? 21 : 22));
            if (depth) {
                CHECK(dref_result_type == image_sampled_type);
            }
            if (const char* directory = std::getenv("EGLANTINE_DEPTH_TEST_OUTPUT")) {
                std::filesystem::create_directories(directory);
                const auto name = std::string{depth ? "depth" : "color"} +
                                  (integer ? "-integer" : "-float") + ".spv";
                std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
                file.write(reinterpret_cast<const char*>(code.data()),
                           static_cast<std::streamsize>(code.size() * sizeof(u32)));
                REQUIRE(file.good());
            }
        }
    }
}
