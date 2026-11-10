// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Shader::Ampere {

/// A native program copied out of a shader container from the game's files.
struct NativeProgramImage {
    std::vector<u8> header; ///< The 0x50-byte program header of a graphics program, else empty
    std::vector<u8> code;   ///< 16 bytes per instruction, as the runtime capture reads them

    /// Program header type 2: pixel; otherwise vertex, tessellation or geometry.
    [[nodiscard]] bool IsPixel() const {
        return !header.empty() && (header[0] & 0x1f) == 2;
    }

    /// Program header type 1 with shader type 1 (bits 10-13): vertex.
    [[nodiscard]] bool IsVertex() const {
        return header.size() >= 2 && (header[0] & 0x1f) == 1 && ((header[1] >> 2) & 0xf) == 1;
    }
};

/// How a compute program is dispatched, from its shader control section.
struct NativeComputeDispatch {
    u32 program; ///< Index into NativeProgramSet::programs
    std::array<u32, 3> workgroup;
    u32 shared_memory; ///< Bytes, rounded up as dispatches allocate it
};

/// The native programs of an application, the vertex and pixel programs its shader programs
/// combine (indices into programs), and how its compute programs are dispatched.
struct NativeProgramSet {
    std::vector<NativeProgramImage> programs;
    std::vector<std::pair<u32, u32>> pairs;
    std::vector<NativeComputeDispatch> compute;
};

} // namespace Shader::Ampere
