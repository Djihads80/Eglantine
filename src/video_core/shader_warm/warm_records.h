// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ShaderWarm {

/// Precache file records: compute entries in the disk pipeline cache format, with no recorded
/// reads (the emulator guesses them, see GuessMissing).

/// Precache files use the Vulkan pipeline cache version: the emulator loads one only when it
/// matches, so the two must move together (vk_pipeline_cache.cpp checks it).
constexpr u32 WarmCacheVersion = 34;

/// What the builder finds in the game files, recorded in the precache's count file. A file built by
/// an older builder is rebuilt at boot (RefreshWarmFiles). 2: Mario Kart World's .esetb effects.
/// 3: .ptcl particle effects (Fire Emblem).
constexpr u32 WarmBuilderVersion = 3;

using ComputeKey = std::array<u8, 24>;

/// A compute program lowered and laid out like GenericEnvironment::LowerNativeCode.
struct LoweredCode {
    std::vector<u8> code;
    /// Constant buffer the program's bound textures read, ~0U when it names none.
    u32 texture_buffer{~0U};
};

/// Captures and lowers one extracted compute program as the runtime does for a dispatch and lays
/// it out with LayOutLoweredNativeCode (a zero shader header). Matrix multiply programs, lowered
/// per title at runtime, are not converted. Returns the error text when the program cannot be
/// lowered.
std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out);

/// The pipeline cache key hash of laid out code (NativeShaderHash).
u64 StageHash(std::span<const u8> code);

struct ComputeControl {
    std::array<u32, 3> workgroup{};
    u32 shared_memory{};
    u32 local_memory{};
};

/// Workgroup (control bytes 88, 92, 96), shared memory (byte 100 rounded up to 256) and local
/// memory (byte 104). Empty when the control is too short.
std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control);

/// One compute entry (env count, env, code, compute trailer, key); `key` gets the 24-byte key.
std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key);

} // namespace VideoCommon::ShaderWarm
