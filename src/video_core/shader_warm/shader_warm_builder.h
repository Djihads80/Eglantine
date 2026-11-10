// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace VideoCommon::ShaderWarm {

/// Where the shader precache file of a title lives: next to its pipeline cache
/// (<shader dir>/<title id>/warm-compute.bin). It holds the compute programs found in the game
/// files, in pipeline cache format with no recorded reads.
std::filesystem::path ComputeWarmPath(u64 title_id);

/// "Already precached" marker of a precache file (same folder, extension .done), written by the
/// emulator after the precache ran to the end. A matching marker skips the precache at boot.
std::filesystem::path WarmMarkerPath(const std::filesystem::path& warm_file);

/// Entry count of a precache file, written by the builder next to it (extension .count): the
/// entry count and the file's size. The file format has no count, so the loading screen shows
/// exact progress only with this.
std::filesystem::path WarmCountPath(const std::filesystem::path& warm_file);

/// The entry count recorded for `warm_file`, or 0 when there is none or it was written for a
/// file of another size.
u64 ReadWarmCount(const std::filesystem::path& warm_file);

/// What a marker records: the precache file's size and last-write time, its entry count, the GPU
/// driver version and device ID and the pipeline cache version.
struct WarmStamp {
    u64 size{};
    s64 write_time{};
    u64 entries{};
    u32 driver{};
    u32 device_id{};
    u32 cache_version{};

    /// True when `other` was written for the same file, driver, device and cache version (the
    /// entry count is informative).
    [[nodiscard]] bool Matches(const WarmStamp& other) const noexcept {
        return size == other.size && write_time == other.write_time && driver == other.driver &&
               device_id == other.device_id && cache_version == other.cache_version;
    }
};

/// The stamp of `warm_file` as it is now, empty when its size or time cannot be read.
std::optional<WarmStamp> CurrentWarmStamp(const std::filesystem::path& warm_file, u64 entries,
                                          u32 driver, u32 device_id, u32 cache_version);

/// The marker of `warm_file`, empty when there is none or it cannot be read.
std::optional<WarmStamp> ReadWarmMarker(const std::filesystem::path& warm_file);

/// Writes the marker of `warm_file`. Returns false when it cannot be written.
bool WriteWarmMarker(const std::filesystem::path& warm_file, const WarmStamp& stamp);

/// True when the title has a precache file.
bool HasWarmFile(u64 title_id);

/// Removes the title's precache file, its marker and count and a leftover temporary file.
/// Returns false when a file exists but cannot be removed, with the reason in `error` when given.
bool DeleteWarmFiles(u64 title_id, std::string* error = nullptr);

struct Progress {
    std::string stage;   ///< "Reading game files", "Extracting shaders", "Converting", "Writing"
    size_t done{};       ///< Items done in this stage
    size_t total{};      ///< Items in this stage, 0 when unknown
    /// The whole build, 0 to 1: archive bytes read, weighted with programs converted. Never
    /// decreases.
    double fraction{};
    size_t programs{};   ///< Compute programs found so far
};

struct Result {
    bool ok{};
    std::string error;           ///< Empty when ok
    size_t archives{};           ///< Shader archives found in the game files
    size_t compute_programs{};   ///< Entries written to the precache file
    size_t skipped{};            ///< Programs that failed to extract or convert
};

/// Builds the precache file for the game at `game_path` (any format the loader opens, Switch 2
/// .nup first), from the user's own copy and keys, with no external tools. Runs on the calling
/// thread; `progress` is called from it, at most every 50 ms; stops early when `stop` is
/// requested.
Result BuildWarmFiles(Core::System& system, const std::filesystem::path& game_path, u64 title_id,
                      const std::function<void(const Progress&)>& progress,
                      std::stop_token stop);

/// Overrides for BuildWarmFilesWith (tests and tools).
struct BuildOptions {
    /// Folder for warm-compute.bin; empty: <shader dir>/<title id>.
    std::filesystem::path output_dir;
    /// Conversion threads, 0: a quarter of the CPU threads, 2 to 6.
    size_t threads{};
};

/// BuildWarmFiles with an explicit output folder.
Result BuildWarmFilesWith(Core::System& system, const std::filesystem::path& game_path,
                          u64 title_id, const BuildOptions& options,
                          const std::function<void(const Progress&)>& progress,
                          std::stop_token stop);

} // namespace VideoCommon::ShaderWarm
