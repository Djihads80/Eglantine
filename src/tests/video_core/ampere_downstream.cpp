// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
namespace IR = Shader::IR;
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

class LoweredEnvironment final : public Shader::Environment {
public:
    explicit LoweredEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return code[i];
        }
        // Scheduling words between the lowered instructions.
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Buffer; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::R32_UINT;
    }
    bool IsTexturePixelFormatInteger(u32) override { return true; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 3; }
    u32 LocalMemorySize() const override { return 0x1000; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool IsNativeProgram() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
private:
    std::span<const u64> code;
};

// Lowers, translates through the Maxwell frontend and emits SPIR-V.
void Translate(const std::vector<Instruction>& native) {
    const auto lowered = LowerToMaxwell(native);
    INFO("lowering error: " << lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    LoweredEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::HostTranslateInfo host;
    host.support_int64 = true;
    host.support_float16 = true;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_int8 = true;
    profile.support_int16 = true;
    const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    CHECK(words.size() > 5);
}

// MOV Rd, c[3][offset], a descriptor handle source the texture pass can track.
constexpr Instruction HandleLoad(u32 reg, u32 offset) {
    return {{0x00007a02U | (reg << 16), (3U << 22) | (offset << 6), 0x00000f00, 0}};
}
} // namespace

TEST_CASE("Newly lowered Ampere forms translate through the Maxwell frontend to SPIR-V",
          "[ampere][downstream][spirv]") {
    const std::vector<std::pair<std::string, std::vector<Instruction>>> programs{
        {"IMMA signed int8", {{{{0x28287237, 0x2e, 0x00405cff, 0}}, Exit}}},
        {"global compare exchange", {{{{0x060873a9, 0x0a, 0x001ee10b, 0}}, Exit}}},
        {"packed half reduction", {{{{0x00067a02, 3U << 22, 0xf00, 0}},
            {{0x00077a02, (3U << 22) | (4U << 6), 0xf00, 0}},
            {{0x0600798e, 0x13, 0x0010e900, 0}}, Exit}}},
        {"IMAD.HI.U32 cbuf pair", {{{{0x402c7627, 0x00400000, 0x078e0027, 0x040fe200}}, Exit}}},
        {"IMAD.WIDE.U32.X", {{{{0x0f0a7225, 0x000000ff, 0x010e040a, 0x003fde00}}, Exit}}},
        {"IMAD.X cbuf", {{{{0x161d7624, 0x01004600, 0x038e061d, 0x000fe200}}, Exit}}},
        {"LEA negated cbuf", {{{{0x0c007a11, 0x80c00a00, 0x078e18ff, 0x001fc400}}, Exit}}},
        {"I2F.F16.U32", {{{{0x00157306, 0x00000007, 0x00200800, 0x000fe400}}, Exit}}},
        {"F2I.U32.F16.FLOOR", {{{{0x00207305, 0x1000001e, 0x00107000, 0x0002b000}}, Exit}}},
        {"guarded ULEA carry", {{{{0x04053291, 0x00000006, 0x0f80203f, 0x000fe200}}, Exit}}},
        {"guarded USHF.R.U64", {{{{0x04043299, 0x00000006, 0x08001205, 0x001fc600}}, Exit}}},
        {"BMSK", {{{{0x0405721b, 0x00000005, 0x00000000, 0x000fe400}}, Exit}}},
        {"BMSK.W", {{{{0x0405721b, 0x00000005, 0x00000800, 0x000fe400}}, Exit}}},
        {"LOP3 predicate OR", {{{{0x01ff7a12, 0x00000500, 0x0000fcff, 0x000fda00}}, Exit}}},
        {"LOP3 predicate PAND", {{{{0x01ff7212, 0x00000002, 0x0003fcff, 0x000fda00}}, Exit}}},
        {"LDL.128 uniform base", {{{{0xff807983, 0x00034005, 0x08100c00, 0x000ea200}}, Exit}}},
        {"SULD STRONG.SYS", {{HandleLoad(8, 0x10), {{0x07077998, 0x20000000, 0x001f4108, 0x008f6400}}, Exit}}},
        {"SUST STRONG.SYS", {{HandleLoad(6, 0x10), {{0x1000799c, 0x20000008, 0x00114f06, 0x0041e200}}, Exit}}},
        {"SEL shadowed", {{{{0xfafa7207, 0x000000ff, 0x02000000, 0x000fe400}}, Exit}}},
        {"STG shadowed", {{{{0x66007386, 0x000000f6, 0x0010a900, 0x004fe800}}, Exit}}},
        {"F2F.F16.F32 negated cbuf", {{{{0x00137b04, 0x80c01000, 0x00200800, 0x001fe200}}, Exit}}},
        {"IPA.PASS high attribute", {{{{0x00097326, 0x000000ff, 0x000e00ee, 0x000e2200}}, Exit}}},
        {"STL.128 negative offset", {{{{0x08007387, 0xfffea00c, 0x00100c00, 0x0001e800}}, Exit}}},
        {"ARRAY_2D.DC banks 4 and 9",
         {{{{0x20227b60, 0xb1012422, 0x041e41ff, 0x000fc200}}, {{0x14147b60, 0xb241381e, 0x041e41ff, 0x00814400}},
           Exit}}},
        {"TMML second buffer",
         {{{{0x00157b69, 0x210000ff, 0x040002ff, 0x000f4200}}, {{0x00157b69, 0x22c000ff, 0x040002ff, 0x000f4200}},
           Exit}}},
        {"TXQ second buffer",
         {{{{0x07027b6f, 0x0100e000, 0x000003ff, 0x000f4200}}, {{0x07027b6f, 0x02c0e000, 0x000003ff, 0x000f4200}},
           Exit}}},
        {"return pair epilogue",
         {{{{0x000d7802, 0x20, 0x00000f00, 0}}, {{0x00007944, 0x20, 0x03c00000, 0}}, Exit,
           {{0x00007918, 0, 0, 0}}, {{0x000c7202, 0x0000000d, 0x00000f00, 0}},
           {{0x000d7802, 0x00000000, 0x00000f00, 0}}, {{0x0c007950, 0xffffff90, 0x03c3ffff, 0}}}}},
    };
    for (const auto& [name, native] : programs) {
        INFO(name);
        CHECK_NOTHROW(Translate(native));
    }
}
