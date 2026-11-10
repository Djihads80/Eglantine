// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shader precompilation (PipelineCache::PrecompileNativePrograms): what the pipeline cache's
// recorded pipelines tell it, and the files it keeps beside the pipeline cache.

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ampere/native_program_image.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Vulkan {

class Device;

using AttributeLayout = std::array<u8, Maxwell::NumVertexAttributes>;

/// The fixed state beyond the render setup that shapes a pipeline's SPIR-V (alpha test, transform
/// feedback and attribute types are set for precompiled pipelines).
[[nodiscard]] u64 ShaderStateHash(const FixedPipelineState& state);

/// What NVIDIA's compiled pixel code depends on beyond its SPIR-V: the render targets.
[[nodiscard]] u64 RenderSetupHash(const FixedPipelineState& state);

/// The attribute type as a vertex program reads it: normalized formats read as float.
[[nodiscard]] u8 ShaderAttributeType(u32 type);

/// Enables every vertex attribute with the given types (0: float).
void SetVertexTypes(FixedPipelineState& state, const AttributeLayout& types);

/// Identifies a list of native programs by their headers and code.
[[nodiscard]] u64 ProgramListHash(std::span<const Shader::Ampere::NativeProgramImage> programs);

/// How long the precompile waits between checkpoints that rewrite a pipeline cache of this size.
[[nodiscard]] std::chrono::seconds PrecompileCheckpointInterval(size_t cache_size);

/// A render setup of the recorded pipelines.
struct PrecompileSetup {
    /// The first recorded pipeline with this render setup and the shader state most of them have
    GraphicsPipelineCacheKey key;
    u64 render_setup{}; ///< RenderSetupHash of the setup
    u64 shader_state{}; ///< ShaderStateHash of key
    size_t count{};     ///< Recorded pipelines with this render setup
    size_t key_count{}; ///< Those with key's shader state
    /// Recorded pipelines and the first one by shader state hash
    std::unordered_map<u64, std::pair<size_t, GraphicsPipelineCacheKey>> shader_states;
};

/// What the pipelines in the pipeline cache tell the precompile: the render setups and attribute
/// types the game draws with, and which vertex and pixel programs it pairs.
struct PrecompileRecords {
    /// Notes a recorded pipeline; only vertex and pixel pipelines give the fixed state.
    void Note(const GraphicsPipelineCacheKey& key);

    /// The render setups enough recorded pipelines use, most used first.
    [[nodiscard]] std::vector<const PrecompileSetup*> CommonSetups() const;

    std::unordered_map<u64, PrecompileSetup> setups; ///< By render setup hash
    /// Recorded vertex attribute types as programs read them (0: disabled), and how many pipelines
    std::map<AttributeLayout, size_t> layouts;
    size_t keys{};                       ///< Recorded vertex and pixel pipelines
    std::set<std::pair<u64, u64>> pairs; ///< Vertex and pixel shader hashes
};

/// The attribute type combinations vertex programs are precompiled with: the type recorded most at
/// each location, and where recorded pipelines differ (bone indices are integers in skinned meshes,
/// floats in others), the combinations most used there.
struct PrecompileLayouts {
    [[nodiscard]] static PrecompileLayouts Rank(const std::map<AttributeLayout, size_t>& recorded);

    /// The distinct layouts a vertex program reading these locations can see, majority first.
    [[nodiscard]] std::vector<AttributeLayout> For(u32 read_locations) const;

    AttributeLayout majority{};
    u32 varying{};                       ///< Locations where a second type is common
    std::vector<AttributeLayout> ranked; ///< Recorded combinations, most used first
};

/// What the precompile's first pass found in each program.
struct PrecompileProgramInfo {
    explicit PrecompileProgramInfo(size_t count)
        : hashes(count), reads(count), uses_shared(count) {}

    /// Shader hash, as pipeline keys hold it (0: the lowering or the recompiler rejects it, or it
    /// is no vertex, pixel or compute program)
    std::vector<u64> hashes;
    std::vector<u32> reads;      ///< Vertex attribute locations each vertex program reads
    std::vector<u8> uses_shared; ///< Compute programs that touch shared memory
};

/// Identifies the precompile's files: the pipeline cache version and the build, device, driver and
/// dynamic state they were written with.
struct PrecompileFileIdentity {
    u32 version{};
    u64 identity{};
};

/// precompiled.bin: key hashes of the pipelines already precompiled. Empty when the file is
/// missing, damaged or written with another identity.
[[nodiscard]] std::unordered_set<u64> ReadPrecompiledRecords(const std::filesystem::path& path,
                                                             PrecompileFileIdentity identity);
void WritePrecompiledRecords(const std::filesystem::path& path, PrecompileFileIdentity identity,
                             const std::unordered_set<u64>& done);

/// precompile_programs.bin: the first pass's findings for a program list. Fills info (sized for
/// the list) and returns true when the file holds them; otherwise clears info.
[[nodiscard]] bool ReadPrecompileProgramInfo(const std::filesystem::path& path,
                                             PrecompileFileIdentity identity, u64 program_list,
                                             PrecompileProgramInfo& info);
void WritePrecompileProgramInfo(const std::filesystem::path& path, PrecompileFileIdentity identity,
                                u64 program_list, const PrecompileProgramInfo& info);

/// precompile_running.bin: each precompile step is noted before it starts and after it ends, so a
/// step that takes the process down (a driver or Sirit abort) is found on the next boot. It is
/// tried once more, since a kill or power loss leaves steps unfinished too, and skipped once a
/// second boot ended during it, instead of crashing every boot. Pipelines the driver failed to
/// build are left unfinished the same way. The file is kept while it names such steps, for the
/// identity it was written with.
class PrecompileRunLog {
public:
    explicit PrecompileRunLog(std::filesystem::path path, u64 identity);
    ~PrecompileRunLog();

    PrecompileRunLog(const PrecompileRunLog&) = delete;
    PrecompileRunLog& operator=(const PrecompileRunLog&) = delete;

    /// Two earlier boots ended while compiling it, or failed to build it
    [[nodiscard]] bool Skipped(u64 key) const {
        return skipped.contains(key);
    }

    [[nodiscard]] size_t SkippedCount() const {
        return skipped.size();
    }

    /// Steps the last boot left unfinished, tried once more by this one
    [[nodiscard]] size_t RetriedCount() const {
        return retried.size();
    }

    void Start(u64 key) {
        Write(key, Kind::Start);
    }

    void End(u64 key) {
        Write(key, Kind::End);
    }

private:
    enum class Kind : u64 { Start, End, Retried, Skipped };

    struct Record {
        u64 key;
        Kind kind;
    };

    void Write(u64 key, Kind kind);

    std::filesystem::path path;
    std::unordered_set<u64> retried;
    std::unordered_set<u64> skipped;
    std::unordered_set<u64> unfinished; ///< Started by this boot and not ended
    std::mutex mutex;
    std::ofstream file;
};

// Shared with the rest of the pipeline cache (vk_pipeline_cache.cpp).

/// The pipeline cache version, which the precompile's files carry.
[[nodiscard]] u32 PipelineCacheVersion();

/// How many threads build pipelines at runtime.
[[nodiscard]] size_t PipelineBuildThreads(const Device& device);

/// Whether built pipelines outlive the process without Eden's pipeline cache.
[[nodiscard]] bool DriverKeepsPipelines(const Device& device);

} // namespace Vulkan
