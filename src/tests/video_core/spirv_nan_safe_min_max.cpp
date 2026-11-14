// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace {
namespace IR = Shader::IR;

constexpr u32 OpExtInstImport = 11;
constexpr u32 OpExtInst = 12;
constexpr u32 GLSLstd450FMin = 37;
constexpr u32 GLSLstd450FMax = 40;
constexpr u32 GLSLstd450FClamp = 43;
constexpr u32 GLSLstd450NMin = 79;
constexpr u32 GLSLstd450NMax = 80;
constexpr u32 GLSLstd450NClamp = 81;

struct GlslOpCounts {
    size_t fmin{};
    size_t fmax{};
    size_t fclamp{};
    size_t nmin{};
    size_t nmax{};
    size_t nclamp{};
};

GlslOpCounts CountGlslOps(const std::vector<u32>& code) {
    constexpr std::string_view glsl{"GLSL.std.450"};
    u32 glsl_set{};
    GlslOpCounts counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count = code[offset] >> 16;
        REQUIRE(word_count > 0);
        REQUIRE(offset + word_count <= code.size());
        const u32 opcode = code[offset] & 0xffff;
        if (opcode == OpExtInstImport && word_count >= 3) {
            const auto* name = reinterpret_cast<const char*>(&code[offset + 2]);
            if (std::strncmp(name, glsl.data(), glsl.size()) == 0) {
                glsl_set = code[offset + 1];
            }
        } else if (opcode == OpExtInst && word_count >= 5 && code[offset + 3] == glsl_set) {
            switch (code[offset + 4]) {
            case GLSLstd450FMin:
                ++counts.fmin;
                break;
            case GLSLstd450FMax:
                ++counts.fmax;
                break;
            case GLSLstd450FClamp:
                ++counts.fclamp;
                break;
            case GLSLstd450NMin:
                ++counts.nmin;
                break;
            case GLSLstd450NMax:
                ++counts.nmax;
                break;
            case GLSLstd450NClamp:
                ++counts.nclamp;
                break;
            default:
                break;
            }
        }
        offset += word_count;
    }
    return counts;
}

struct Fixture {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Shader::Profile profile{};

    Fixture() {
        program.stage = Shader::Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010300;
    }

    std::vector<u32> Emit() {
        return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    }
};

void CheckNaNDefined(const GlslOpCounts& counts, size_t nmin, size_t nmax) {
    CHECK(counts.fmin == 0);
    CHECK(counts.fmax == 0);
    CHECK(counts.fclamp == 0);
    CHECK(counts.nclamp == 0);
    CHECK(counts.nmin == nmin);
    CHECK(counts.nmax == nmax);
}
} // Anonymous namespace

TEST_CASE("SPIR-V float min, max and clamp emit NaN-defined instructions",
          "[video_core][spirv][nan_safe_min_max]") {
    for (const bool broken_clamp : {false, true}) {
        CAPTURE(broken_clamp);
        Fixture fixture;
        fixture.profile.has_broken_spirv_clamp = broken_clamp;
        fixture.program.info.uses_fp64 = true;
        fixture.program.info.uses_local_invocation_id = true;
        IR::IREmitter ir{fixture.block};
        const IR::F32 a{ir.BitCast<IR::F32>(ir.LocalInvocationIdX())};
        const IR::F32 b{ir.BitCast<IR::F32>(ir.LocalInvocationIdY())};
        const IR::F32 c{ir.BitCast<IR::F32>(ir.LocalInvocationIdZ())};
        const IR::F64 d{ir.PackDouble2x32(
            ir.CompositeConstruct(ir.LocalInvocationIdX(), ir.LocalInvocationIdY()))};
        const IR::F64 e{ir.PackDouble2x32(
            ir.CompositeConstruct(ir.LocalInvocationIdY(), ir.LocalInvocationIdZ()))};
        std::ignore = ir.FPMin(a, b);
        std::ignore = ir.FPMax(a, b);
        std::ignore = ir.FPMin(d, e);
        std::ignore = ir.FPMax(d, e);
        std::ignore = ir.FPSaturate(a);
        std::ignore = ir.FPClamp(a, b, c);
        // Min32 and min64, plus the min half of one saturate and one clamp; likewise for max.
        CheckNaNDefined(CountGlslOps(fixture.Emit()), 4, 4);
    }
}

TEST_CASE("SPIR-V float atomic min and max loops emit NaN-defined instructions",
          "[video_core][spirv][nan_safe_min_max]") {
    Fixture fixture;
    fixture.program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
    fixture.program.info.uses_atomic_f32x2_min = true;
    fixture.program.info.uses_atomic_f32x2_max = true;
    CheckNaNDefined(CountGlslOps(fixture.Emit()), 1, 1);
}
