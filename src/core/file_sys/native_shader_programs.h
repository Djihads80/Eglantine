// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <vector>

#include "common/polyfill_thread.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "shader_recompiler/frontend/ampere/native_program_image.h"

namespace Core {
class System;
}

namespace FileSys {

/// The distinct native shader programs stored in an application's RomFS (Switch 2 shader
/// containers, decompressed when they end in .zs), and the vertex and pixel programs its shader
/// program tables combine. The result is kept in cache_file and read from it while the RomFS
/// keeps the same file names and sizes.
[[nodiscard]] Shader::Ampere::NativeProgramSet CollectNativeShaderPrograms(
    const VirtualFile& romfs, const std::filesystem::path& cache_file, std::stop_token stop);

/// Hands the native programs of the running application's RomFS, as the game reads it (update and
/// mods applied), to the renderer, which compiles them on the loading screen (precompile_shaders
/// setting, with the disk shader cache on). Call it before LoadDiskResources; the list is kept in
/// the title's shader cache folder.
void PrecompileApplicationShaders(Core::System& system, u64 program_id);

} // namespace FileSys
