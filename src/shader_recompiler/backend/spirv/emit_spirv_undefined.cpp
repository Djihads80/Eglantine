// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
// Registers read before any write are zero rather than OpUndef, as NVIDIA hardware and drivers
// give them; AMD's OpUndef is whatever the register held. Title 1's glow pass exits before
// writing its colour on some pixels, which AMD blended in as green patches on the road.
// EDEN_NX2_UNDEF_ZERO=0 emits OpUndef again.
bool UndefIsZero() {
    static const bool zero = [] {
        const char* const value = std::getenv("EDEN_NX2_UNDEF_ZERO");
        return value == nullptr || value[0] != '0';
    }();
    return zero;
}
} // Anonymous namespace

Id EmitUndefU1(EmitContext& ctx) {
    return UndefIsZero() ? ctx.false_value : ctx.OpUndef(ctx.U1);
}

Id EmitUndefU8(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU32(EmitContext& ctx) {
    return UndefIsZero() ? ctx.u32_zero_value : ctx.OpUndef(ctx.U32[1]);
}

Id EmitUndefU64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
