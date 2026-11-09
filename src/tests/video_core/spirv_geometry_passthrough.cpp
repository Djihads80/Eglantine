// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"

TEST_CASE("Geometry passthrough masks do not declare stage-specific builtins",
          "[video_core][spirv][geometry_passthrough]") {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Geometry;
    program.output_topology = OutputTopology::TriangleStrip;
    program.output_vertices = 3;
    program.invocations = 1;
    program.is_geometry_passthrough = true;
    program.info.passthrough.mask.set();
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.support_geometry_shader_passthrough = true;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    bool has_position = false;
    for (size_t offset = 5; offset < code.size();) {
        const auto size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffff) == static_cast<u32>(spv::Op::OpDecorate) && size >= 4 &&
            code[offset + 2] == static_cast<u32>(spv::Decoration::BuiltIn)) {
            const auto builtin = static_cast<spv::BuiltIn>(code[offset + 3]);
            CHECK(builtin != spv::BuiltIn::TessCoord);
            CHECK(builtin != spv::BuiltIn::VertexIndex);
            CHECK(builtin != spv::BuiltIn::InstanceIndex);
            CHECK(builtin != spv::BuiltIn::FrontFacing);
            CHECK(builtin != spv::BuiltIn::BaseInstance);
            has_position |= builtin == spv::BuiltIn::Position;
        }
        offset += size;
    }
    CHECK(has_position);
}
