// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>

#include "shader_recompiler/frontend/ampere/program_override.h"

namespace VideoCommon {

// Workaround for a missing ground-height map in this exact shader. The original wave
// amplitudes and water exclusion mask still control the resulting displacement.
inline bool ApplyMkwZeroDepthFallback(std::span<const Shader::Ampere::Instruction> source,
                                      Shader::Ampere::LoweredProgram& program) {
    if (!program || source.size_bytes() != 0x3b0 ||
        Shader::Ampere::NativeProgramKey(source) != 0x5a4f3726590efb17ULL ||
        program.source_instruction_offsets.size() != source.size()) {
        return false;
    }
    constexpr size_t next_source = 0x2d0 / sizeof(Shader::Ampere::Instruction);
    const size_t insert = program.source_instruction_offsets[next_source];
    if (insert > program.instructions.size() ||
        (next_source != 0 && program.source_instruction_offsets[next_source - 1] > insert)) {
        return false;
    }
    // At this point R8=(water-ground)*20, R9=water and R4=saturate(R8).
    // Both R8 and R9 zero identifies the entirely empty pair, including neither
    // real shallows (equal nonzero heights) nor dry ground (negative depth).
    constexpr u64 always = u64{7} << 16;
    constexpr std::array<u64, 3> fallback{
        // LOP.OR R250, R8, R9 (PT destination; no live predicate is changed).
        0x5c40000000000000ULL | always | 250 | (u64{8} << 8) | (u64{9} << 20) |
            (u64{1} << 41) | (u64{7} << 48),
        // ISET.EQ.U32.AND with float-result mode: 1.0 if empty, otherwise 0.
        0x5b50000000000000ULL | always | 250 | (u64{250} << 8) | (u64{255} << 20) |
            (u64{7} << 39) | (u64{1} << 44) | (u64{2} << 49),
        // Empty input has R4=0; other input ORs with zero and stays unchanged.
        0x5c40000000000000ULL | always | 4 | (u64{4} << 8) | (u64{250} << 20) |
            (u64{1} << 41) | (u64{7} << 48),
    };
    program.instructions.insert(program.instructions.begin() + insert, fallback.begin(),
                                fallback.end());
    // This exact shader has no branches or calls; its predicated EXITs need no relocation.
    for (size_t index = next_source; index < program.source_instruction_offsets.size(); ++index) {
        program.source_instruction_offsets[index] += static_cast<u32>(fallback.size());
    }
    return true;
}

} // namespace VideoCommon
