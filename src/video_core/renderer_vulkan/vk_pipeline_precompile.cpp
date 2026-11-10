// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <numeric>
#include <optional>
#include <string_view>

#include "common/cityhash.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/program_header.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_precompile.h"
#include "video_core/shader_environment.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
using Shader::Maxwell::TranslateProgram;

// A render setup is precompiled once this share of the recorded pipelines uses it. Attribute type
// combinations are precompiled once recorded at all, the most recorded first: one short session
// can record few skinned meshes, and RADV does not reuse a float variant for an integer one.
constexpr size_t RecordedSetupPermille = 20;
constexpr size_t MaxRenderSetups = 6;
constexpr size_t MaxAttributeLayouts = 4;
constexpr u64 MaxPrecompiledKeys = u64{1} << 24;
constexpr std::chrono::seconds MinCheckpointInterval{60};

// The precompile_shaders setting compiles the native programs found in the game's files on the
// loading screen (PipelineCache::PrecompileNativePrograms).
bool PrecompileEnabled() {
    return Settings::values.precompile_shaders.GetValue();
}

std::optional<VideoCommon::ScannedEnvironment> MakeScannedEnvironment(
    const Shader::Ampere::NativeProgramImage& image, Shader::Stage stage) {
    std::vector<Shader::Ampere::Instruction> code(image.code.size() /
                                                  sizeof(Shader::Ampere::Instruction));
    std::memcpy(code.data(), image.code.data(), code.size() * sizeof(code[0]));
    return VideoCommon::ScannedEnvironment::Create(code, image.header, stage);
}

template <typename T>
bool ReadValue(std::ifstream& file, T& value) {
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
bool ReadVector(std::ifstream& file, std::vector<T>& data) {
    return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()),
                                       static_cast<std::streamsize>(data.size() * sizeof(T))));
}

template <typename T>
void WriteValue(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
void WriteVector(std::ofstream& file, const std::vector<T>& data) {
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size() * sizeof(T)));
}

// Writes a temporary file and puts it in place once complete, so a crash leaves the old file.
template <typename Func>
void ReplaceFile(const std::filesystem::path& path, Func&& write) {
    auto temporary{path};
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        write(file);
        file.close();
        if (!file) {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
}

} // Anonymous namespace

u64 ShaderStateHash(const FixedPipelineState& state) {
    const bool points = state.topology == Maxwell::PrimitiveTopology::Points;
    const std::array<u32, 6> fields{
        static_cast<u32>(state.topology.Value()),
        static_cast<u32>(state.ndc_minus_one_to_one.Value()),
        static_cast<u32>(state.early_z.Value()),
        static_cast<u32>(state.y_negate.Value()),
        static_cast<u32>(state.attachment0_dual_source_blend.Value()),
        points ? state.point_size : 0,
    };
    return Common::CityHash64(reinterpret_cast<const char*>(fields.data()),
                              fields.size() * sizeof(u32));
}

u64 RenderSetupHash(const FixedPipelineState& state) {
    struct {
        std::array<u8, Maxwell::NumRenderTargets> color_formats;
        u32 depth_enabled;
        u32 depth_format;
        u32 msaa_mode;
    } setup{state.color_formats, state.depth_enabled.Value(), state.depth_format.Value(),
            static_cast<u32>(state.msaa_mode.Value())};
    return Common::CityHash64(reinterpret_cast<const char*>(&setup), sizeof(setup));
}

u8 ShaderAttributeType(u32 type) {
    using Type = Maxwell::VertexAttribute::Type;
    const auto value = static_cast<Type>(type);
    return value == Type::SNorm || value == Type::UNorm ? static_cast<u8>(Type::Float)
                                                        : static_cast<u8>(type);
}

void SetVertexTypes(FixedPipelineState& state, const AttributeLayout& types) {
    // attribute_types shares its bits with enabled_divisors, set instead with static vertex input.
    const bool dynamic = state.dynamic_vertex_input != 0;
    if (dynamic) {
        state.attribute_types = 0;
    }
    for (size_t index = 0; index < state.attributes.size(); ++index) {
        auto& attribute = state.attributes[index];
        attribute.raw = 0;
        attribute.enabled.Assign(1);
        attribute.type.Assign(types[index]);
        // Scaled formats exist only up to 16 bits a component.
        using Type = Maxwell::VertexAttribute::Type;
        using Size = Maxwell::VertexAttribute::Size;
        const bool scaled = types[index] == static_cast<u8>(Type::UScaled) ||
                            types[index] == static_cast<u8>(Type::SScaled);
        attribute.size.Assign(
            static_cast<u32>(scaled ? Size::Size_R8_G8_B8_A8 : Size::Size_R32_G32_B32_A32));
        if (dynamic) {
            const u64 dynamic_type = types[index] == static_cast<u8>(Type::SInt)   ? 2
                                     : types[index] == static_cast<u8>(Type::UInt) ? 3
                                                                                   : 1;
            state.attribute_types |= dynamic_type << (index * 2);
        }
    }
}

u64 ProgramListHash(std::span<const Shader::Ampere::NativeProgramImage> programs) {
    u64 hash = programs.size();
    for (const auto& image : programs) {
        hash = Common::CityHash64WithSeed(reinterpret_cast<const char*>(image.header.data()),
                                          image.header.size(), hash);
        hash = Common::CityHash64WithSeed(reinterpret_cast<const char*>(image.code.data()),
                                          image.code.size(), hash);
    }
    return hash;
}

std::chrono::seconds PrecompileCheckpointInterval(size_t cache_size) {
    // A second for each megabyte rewritten, as the runtime flushes.
    return std::max(MinCheckpointInterval,
                    std::chrono::seconds{static_cast<s64>(cache_size / (1024 * 1024))});
}

void PrecompileRecords::Note(const GraphicsPipelineCacheKey& key) {
    // Vertex and pixel pipelines without tessellation or geometry give the fixed state.
    if (key.unique_hashes[0] != 0 || key.unique_hashes[1] == 0 || key.unique_hashes[2] != 0 ||
        key.unique_hashes[3] != 0 || key.unique_hashes[4] != 0 || key.unique_hashes[5] == 0) {
        return;
    }
    const u64 render_setup = RenderSetupHash(key.state);
    auto& setup = setups[render_setup];
    setup.render_setup = render_setup;
    ++setup.count;
    // The key changes only when another shader state takes the lead, so pipelines recorded in
    // later sessions do not change what earlier boots precompiled. A setup is precompiled with the
    // shader state most of its recorded pipelines have, not a point list or dual-source draw that
    // came first.
    const u64 state = ShaderStateHash(key.state);
    auto& [same, first] = setup.shader_states.try_emplace(state, size_t{0}, key).first->second;
    ++same;
    if (setup.count > 1 && state == setup.shader_state) {
        setup.key_count = same;
    } else if (same > setup.key_count) {
        setup.key = first;
        setup.shader_state = state;
        setup.key_count = same;
    }
    ++keys;
    pairs.emplace(key.unique_hashes[1], key.unique_hashes[5]);
    // With dynamic vertex input the key holds only the type class (1 float, 2 signed and 3
    // unsigned integer, 0 disabled), not the attributes.
    using Type = Maxwell::VertexAttribute::Type;
    static constexpr std::array DynamicTypes{u8{0}, static_cast<u8>(Type::Float),
                                             static_cast<u8>(Type::SInt),
                                             static_cast<u8>(Type::UInt)};
    AttributeLayout layout{};
    for (size_t index = 0; index < key.state.attributes.size(); ++index) {
        const auto& attribute = key.state.attributes[index];
        if (key.state.dynamic_vertex_input) {
            layout[index] = DynamicTypes[key.state.DynamicAttributeType(index)];
        } else if (attribute.enabled) {
            layout[index] = ShaderAttributeType(attribute.type.Value());
        }
    }
    ++layouts[layout];
}

std::vector<const PrecompileSetup*> PrecompileRecords::CommonSetups() const {
    std::vector<const PrecompileSetup*> common;
    for (const auto& [hash, setup] : setups) {
        if (setup.count * 1000 >= keys * RecordedSetupPermille) {
            common.push_back(&setup);
        }
    }
    std::ranges::sort(common, [](const PrecompileSetup* lhs, const PrecompileSetup* rhs) {
        return lhs->count > rhs->count;
    });
    if (common.size() > MaxRenderSetups) {
        common.resize(MaxRenderSetups);
    }
    return common;
}

PrecompileLayouts PrecompileLayouts::Rank(const std::map<AttributeLayout, size_t>& recorded) {
    PrecompileLayouts result;
    std::array<std::array<size_t, 8>, Maxwell::NumVertexAttributes> type_counts{};
    for (const auto& [layout, count] : recorded) {
        for (size_t index = 0; index < layout.size(); ++index) {
            if (layout[index] != 0) {
                type_counts[index][layout[index] & 7] += count;
            }
        }
    }
    for (size_t index = 0; index < result.majority.size(); ++index) {
        const auto& counts = type_counts[index];
        const size_t enabled = std::accumulate(counts.begin(), counts.end(), size_t{0});
        result.majority[index] = static_cast<u8>(Maxwell::VertexAttribute::Type::Float);
        size_t best = 0;
        size_t second = 0;
        for (u8 type = 1; type < counts.size(); ++type) {
            if (counts[type] > best) {
                second = best;
                best = counts[type];
                result.majority[index] = type;
            } else if (counts[type] > second) {
                second = counts[type];
            }
        }
        if (second != 0) {
            result.varying |= 1U << index;
            LOG_INFO(Render_Vulkan,
                     "Precompile: attribute {} recorded as type {} in {} of {} pipelines and as "
                     "another type in {}",
                     index, result.majority[index], best, enabled, enabled - best);
        }
    }
    std::map<AttributeLayout, size_t> projected; ///< Recorded layouts on the varying locations
    for (const auto& [layout, count] : recorded) {
        AttributeLayout variant = result.majority;
        for (size_t index = 0; index < layout.size(); ++index) {
            if ((result.varying >> index) & 1 && layout[index] != 0) {
                variant[index] = layout[index];
            }
        }
        projected[variant] += count;
    }
    std::vector<std::pair<size_t, AttributeLayout>> ranked;
    for (const auto& [layout, count] : projected) {
        ranked.emplace_back(count, layout);
    }
    std::ranges::sort(ranked, std::greater{});
    if (ranked.size() > MaxAttributeLayouts) {
        ranked.resize(MaxAttributeLayouts);
    }
    for (const auto& [count, layout] : ranked) {
        result.ranked.push_back(layout);
    }
    return result;
}

std::vector<AttributeLayout> PrecompileLayouts::For(u32 read_locations) const {
    std::vector<AttributeLayout> layouts{majority};
    const u32 mask = read_locations & varying;
    for (const auto& layout : ranked) {
        AttributeLayout variant = majority;
        for (size_t index = 0; index < layout.size(); ++index) {
            if ((mask >> index) & 1) {
                variant[index] = layout[index];
            }
        }
        if (std::ranges::find(layouts, variant) == layouts.end()) {
            layouts.push_back(variant);
        }
    }
    return layouts;
}

std::unordered_set<u64> ReadPrecompiledRecords(const std::filesystem::path& path,
                                               PrecompileFileIdentity identity) {
    std::ifstream file(path, std::ios::binary);
    u32 version{};
    u64 stored{};
    u64 count{};
    if (!ReadValue(file, version) || version != identity.version || !ReadValue(file, stored) ||
        stored != identity.identity || !ReadValue(file, count) || count > MaxPrecompiledKeys) {
        return {};
    }
    std::vector<u64> hashes(count);
    if (!ReadVector(file, hashes)) {
        return {};
    }
    return {hashes.begin(), hashes.end()};
}

void WritePrecompiledRecords(const std::filesystem::path& path, PrecompileFileIdentity identity,
                             const std::unordered_set<u64>& done) {
    ReplaceFile(path, [&](std::ofstream& file) {
        WriteValue(file, identity.version);
        WriteValue(file, identity.identity);
        WriteValue(file, static_cast<u64>(done.size()));
        for (const u64 hash : done) {
            WriteValue(file, hash);
        }
    });
}

bool ReadPrecompileProgramInfo(const std::filesystem::path& path, PrecompileFileIdentity identity,
                               u64 program_list, PrecompileProgramInfo& info) {
    std::ifstream file(path, std::ios::binary);
    u32 version{};
    u64 stored{};
    u64 list{};
    u64 count{};
    const bool valid = ReadValue(file, version) && version == identity.version &&
                       ReadValue(file, stored) && stored == identity.identity &&
                       ReadValue(file, list) && list == program_list && ReadValue(file, count) &&
                       count == info.hashes.size() && ReadVector(file, info.hashes) &&
                       ReadVector(file, info.reads) && ReadVector(file, info.uses_shared);
    if (!valid) {
        // Cleared for the first pass, which sets only what it finds.
        std::ranges::fill(info.hashes, u64{0});
        std::ranges::fill(info.reads, u32{0});
        std::ranges::fill(info.uses_shared, u8{0});
    }
    return valid;
}

void WritePrecompileProgramInfo(const std::filesystem::path& path, PrecompileFileIdentity identity,
                                u64 program_list, const PrecompileProgramInfo& info) {
    ReplaceFile(path, [&](std::ofstream& file) {
        WriteValue(file, identity.version);
        WriteValue(file, identity.identity);
        WriteValue(file, program_list);
        WriteValue(file, static_cast<u64>(info.hashes.size()));
        WriteVector(file, info.hashes);
        WriteVector(file, info.reads);
        WriteVector(file, info.uses_shared);
    });
}

PrecompileRunLog::PrecompileRunLog(std::filesystem::path path_, u64 identity)
    : path{std::move(path_)} {
    {
        std::ifstream old_file(path, std::ios::binary);
        u64 stored{};
        Record record{};
        std::unordered_set<u64> running;
        std::unordered_set<u64> unfinished_once;
        if (ReadValue(old_file, stored) && stored == identity) {
            while (ReadValue(old_file, record)) {
                switch (record.kind) {
                case Kind::Start:
                    running.insert(record.key);
                    break;
                case Kind::End:
                    running.erase(record.key);
                    retried.erase(record.key);
                    break;
                case Kind::Retried:
                    retried.insert(record.key);
                    break;
                case Kind::Skipped:
                    skipped.insert(record.key);
                    break;
                }
            }
        }
        for (const u64 key : running) {
            if (retried.erase(key) != 0) {
                skipped.insert(key);
            } else {
                unfinished_once.insert(key);
            }
        }
        retried.merge(unfinished_once);
    }
    file.open(path, std::ios::binary | std::ios::trunc);
    WriteValue(file, identity);
    for (const u64 key : retried) {
        Write(key, Kind::Retried);
    }
    for (const u64 key : skipped) {
        Write(key, Kind::Skipped);
    }
}

PrecompileRunLog::~PrecompileRunLog() {
    file.close();
    // Kept for steps carried over or left unfinished by this boot (a failed build).
    if (retried.empty() && skipped.empty() && unfinished.empty()) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}

void PrecompileRunLog::Write(u64 key, Kind kind) {
    const Record record{key, kind};
    std::scoped_lock lock{mutex};
    if (kind == Kind::Start) {
        unfinished.insert(key);
    } else if (kind == Kind::End) {
        unfinished.erase(key);
    }
    WriteValue(file, record);
    // Handed to the system at once, so the record outlasts a crash of the process.
    file.flush();
}

/// A pipeline the precompile builds.
struct PrecompileJob {
    u32 vertex; ///< Or the compute program
    u32 pixel;
    u32 base_key;
    ComputePipelineCacheKey compute_key; ///< Compute jobs: unique_hash != 0
    u64 done;                            ///< What precompiled.bin records for the job
};

/// The precompile's jobs. Graphics jobs name their key's fixed state (1 KB) in base_keys, one per
/// render setup and attribute layout, rather than hold it: Mario Kart World queues 67k jobs.
struct PrecompileJobList {
    [[nodiscard]] GraphicsPipelineCacheKey Key(const PrecompileJob& job,
                                               std::span<const u64> hashes) const {
        GraphicsPipelineCacheKey key = base_keys[job.base_key];
        key.unique_hashes = {};
        key.unique_hashes[1] = hashes[job.vertex];
        key.unique_hashes[5] = hashes[job.pixel];
        return key;
    }

    std::vector<PrecompileJob> jobs;
    std::vector<GraphicsPipelineCacheKey> base_keys;
    size_t compute_jobs{};
    size_t render_setups{};
    size_t attribute_layouts{};
    size_t layout_variants{};
};

struct PrecompileBuildCounts {
    size_t built{};
    size_t rejected{}; ///< By the recompiler or the device limits
    size_t failed{};   ///< By the driver
};

void PipelineCache::PrecompileNativePrograms(
    const VideoCore::RasterizerInterface::NativeProgramSource& open) {
    if (!PrecompileEnabled() || !Settings::CurrentProgramIsSwitch2() ||
        precompile_collector.joinable()) {
        return;
    }
    if (!use_vulkan_pipeline_cache && !DriverKeepsPipelines(device)) {
        LOG_INFO(Render_Vulkan, "Precompile skipped: with the Vulkan pipeline cache off and no "
                                "driver cache, precompiled pipelines would not outlast the boot");
        return;
    }
    auto collect = open();
    if (!collect) {
        return;
    }
    precompile_collector = std::jthread([this, collect = std::move(collect)](std::stop_token stop) {
        auto set = collect(stop);
        std::scoped_lock lock{precompile_mutex};
        precompile_set = std::move(set);
    });
}

void PipelineCache::NoteRecordedPipeline(const GraphicsPipelineCacheKey& key) {
    if (PrecompileEnabled()) {
        precompile_records.Note(key);
    }
}

void PipelineCache::RunPrecompile(std::stop_token stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    // Nothing here is used after the loading screen (Mario Kart World: 11,261 programs).
    SCOPE_EXIT {
        precompile_set = {};
        precompile_records = {};
        precompile_done = {};
    };
    if (!precompile_collector.joinable()) {
        return;
    }
    {
        // Shown while the first boot scans the RomFS, instead of the cache load's finished count.
        callback(VideoCore::LoadCallbackStage::Prepare, 0, 0);
        // Leaving the loading screen stops a RomFS scan still running.
        std::stop_callback cancel_scan{stop_loading,
                                       [this] { precompile_collector.request_stop(); }};
        precompile_collector.join();
    }
    if (precompile_set.programs.empty() || stop_loading.stop_requested()) {
        return;
    }
    // Graphics pipelines copy the fixed state of recorded ones; compute pipelines need none.
    const bool graphics = precompile_records.keys != 0;
    if (!graphics) {
        LOG_INFO(Render_Vulkan, "Precompile: the pipeline cache has no graphics pipelines yet; "
                                "graphics shaders are precompiled on the next boot");
    }
    const auto start = std::chrono::steady_clock::now();
    // As many threads as pipeline builds use at runtime.
    const size_t threads = PipelineBuildThreads(device);
    precompile_workers.emplace(threads, "VkPrecompile");
    PrecompileRunLog run_log(PrecompileFile("precompile_running.bin"),
                             PrecompileIdentity().identity);
    if (run_log.RetriedCount() != 0) {
        LOG_INFO(Render_Vulkan,
                 "Precompile: retrying {} programs and pipelines the last boot failed to build or "
                 "was compiling when it ended",
                 run_log.RetriedCount());
    }
    if (run_log.SkippedCount() != 0) {
        LOG_WARNING(Render_Vulkan,
                    "Precompile: skipping {} programs and pipelines two boots failed to build or "
                    "were compiling when they ended",
                    run_log.SkippedCount());
    }

    const PrecompileProgramInfo info =
        PrecompileFirstPass(run_log, graphics, callback, stop_loading);
    LoadPrecompiled();
    const PrecompileJobList list = MakePrecompileJobs(info, run_log, graphics);
    LOG_INFO(
        Render_Vulkan,
        "Precompiling {} pipelines ({} compute): {} programs, {} vertex and pixel pairs, {} "
        "render setups, {} attribute layouts ({} pair variants), {} compiled before, {} threads",
        list.jobs.size(), list.compute_jobs, precompile_set.programs.size(),
        precompile_set.pairs.size(), list.render_setups, list.attribute_layouts,
        list.layout_variants, precompile_done.size(), threads);
    const PrecompileBuildCounts counts =
        BuildPrecompileJobs(list, info, run_log, callback, stop_loading);
    precompile_workers.reset();
    // Saved by LoadDiskResources once the Vulkan cache file is written.
    if (counts.built + counts.rejected > 0) {
        precompile_unsaved = std::move(precompile_done);
    }
    // Jobs left when loading was cancelled are neither built nor failed.
    LOG_INFO(Render_Vulkan,
             "Precompiled {} of {} pipelines in {:.1f} s ({} rejected by the recompiler, {} "
             "failed to build)",
             counts.built, list.jobs.size(),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
             counts.rejected, counts.failed);
}

PrecompileProgramInfo PipelineCache::PrecompileFirstPass(
    PrecompileRunLog& run_log, bool graphics, const VideoCore::DiskResourceLoadCallback& callback,
    std::stop_token stop_loading) {
    const auto& programs = precompile_set.programs;
    PrecompileProgramInfo info(programs.size());
    // Read back when the programs, the build and the device are those of an earlier boot.
    const auto path = PrecompileFile("precompile_programs.bin");
    const u64 program_list = ProgramListHash(programs);
    const bool known =
        !path.empty() && ReadPrecompileProgramInfo(path, PrecompileIdentity(), program_list, info);
    if (known) {
        return info;
    }
    std::mutex pass_mutex;
    size_t passed = 0;
    const size_t pass_total =
        graphics ? programs.size()
                 : static_cast<size_t>(std::ranges::count_if(
                       programs, [](const auto& image) { return image.header.empty(); }));
    for (size_t index = 0; index < programs.size(); ++index) {
        if (!graphics && !programs[index].header.empty()) {
            continue;
        }
        precompile_workers->QueueWork([&, index] {
            SCOPE_EXIT {
                std::scoped_lock lock{pass_mutex};
                callback(VideoCore::LoadCallbackStage::Build, ++passed, pass_total);
            };
            const auto& image = programs[index];
            const bool compute = image.header.empty();
            const bool pixel = image.IsPixel();
            if (!compute && !pixel && !image.IsVertex()) {
                return; // Tessellation and geometry programs: not precompiled
            }
            auto env = MakeScannedEnvironment(image, compute ? Shader::Stage::Compute
                                                     : pixel ? Shader::Stage::Fragment
                                                             : Shader::Stage::VertexB);
            if (!env) {
                return;
            }
            const u64 hash = env->Hash();
            if (run_log.Skipped(hash)) {
                return;
            }
            info.hashes[index] = hash;
            run_log.Start(hash);
            SCOPE_EXIT {
                run_log.End(hash);
            };
            if (compute) {
                // Emitting a shared memory access without shared memory aborts in Sirit, so
                // dispatches that give none are only used for programs that need none.
                try {
                    ShaderPools pools;
                    Shader::Maxwell::Flow::CFG cfg(*env, pools.flow_block, env->StartAddress());
                    const auto program =
                        TranslateProgram(pools.inst, pools.block, *env, cfg, host_info);
                    // A compute program's local memory size comes with its dispatch, which the
                    // game's files do not give, so programs that use it are left out.
                    if (program.info.uses_local_memory) {
                        info.hashes[index] = 0;
                        return;
                    }
                    for (const Shader::IR::Block* const block : program.post_order_blocks) {
                        for (const Shader::IR::Inst& inst : block->Instructions()) {
                            if (Shader::IR::NameOf(inst.GetOpcode()).find("Shared") !=
                                std::string_view::npos) {
                                info.uses_shared[index] = 1;
                            }
                        }
                    }
                } catch (const std::exception&) {
                    info.hashes[index] = 0;
                }
                return;
            }
            // A program the recompiler rejects would fail in every pair it is in.
            try {
                ShaderPools pools;
                const u32 cfg_offset{static_cast<u32>(sizeof(Shader::ProgramHeader))};
                Shader::Maxwell::Flow::CFG cfg(*env, pools.flow_block, cfg_offset, false);
                const auto program =
                    TranslateProgram(pools.inst, pools.block, *env, cfg, host_info);
                if (pixel) {
                    return;
                }
                for (size_t attribute = 0; attribute < Maxwell::NumVertexAttributes; ++attribute) {
                    if (program.info.loads.Generic(attribute)) {
                        info.reads[index] |= 1U << attribute;
                    }
                }
            } catch (const std::exception&) {
                info.hashes[index] = 0;
            }
        });
    }
    precompile_workers->WaitForRequests(stop_loading);
    // Graphics programs are left out without recorded pipelines, so only a full pass is kept.
    if (!path.empty() && graphics && !stop_loading.stop_requested()) {
        WritePrecompileProgramInfo(path, PrecompileIdentity(), program_list, info);
    }
    return info;
}

PrecompileJobList PipelineCache::MakePrecompileJobs(const PrecompileProgramInfo& info,
                                                    const PrecompileRunLog& run_log,
                                                    bool graphics) const {
    const auto& programs = precompile_set.programs;
    const auto& hashes = info.hashes;
    PrecompileJobList list;
    // Compute programs, for each dispatch their shader control sections give. Devices with broken
    // compute build no compute pipelines.
    for (const auto& dispatch : precompile_set.compute) {
        if (device.HasBrokenCompute() || dispatch.program >= programs.size() ||
            hashes[dispatch.program] == 0 ||
            (info.uses_shared[dispatch.program] && dispatch.shared_memory == 0)) {
            continue;
        }
        PrecompileJob job{static_cast<u32>(dispatch.program), 0, 0, {}, 0};
        job.compute_key.unique_hash = hashes[dispatch.program];
        job.compute_key.shared_memory_size = dispatch.shared_memory;
        job.compute_key.workgroup_size = dispatch.workgroup;
        job.done = job.compute_key.Hash();
        if (!precompile_done.contains(job.done) && !run_log.Skipped(job.done)) {
            list.jobs.push_back(job);
            ++list.compute_jobs;
        }
    }
    if (!graphics) {
        return list;
    }

    // Programs in no pair are not built: a pipeline with a stand-in for the other stage is not the
    // one the game draws with.
    std::vector<bool> paired(programs.size());
    std::set<std::pair<u64, u64>> table_pairs;
    for (const auto& [vertex, pixel] : precompile_set.pairs) {
        paired[vertex] = true;
        paired[pixel] = true;
        table_pairs.emplace(hashes[vertex], hashes[pixel]);
    }
    // How well the program tables cover what the game drew before.
    const std::unordered_set<u64> found(hashes.begin(), hashes.end());
    size_t in_tables = 0;
    size_t unpaired_found = 0;
    for (const auto& recorded : precompile_records.pairs) {
        if (table_pairs.contains(recorded)) {
            ++in_tables;
        } else if (found.contains(recorded.first) && found.contains(recorded.second)) {
            ++unpaired_found;
        }
    }
    size_t unpaired_vertex = 0;
    size_t unpaired_pixel = 0;
    for (size_t index = 0; index < programs.size(); ++index) {
        unpaired_vertex += programs[index].IsVertex() && !paired[index] ? 1 : 0;
        unpaired_pixel += programs[index].IsPixel() && !paired[index] ? 1 : 0;
    }
    LOG_INFO(Render_Vulkan,
             "Precompile: {} recorded vertex and pixel pairs, {} in the program tables, {} of "
             "programs found in no such pair; {} vertex and {} pixel programs in no pair",
             precompile_records.pairs.size(), in_tables, unpaired_found, unpaired_vertex,
             unpaired_pixel);

    const auto setups = precompile_records.CommonSetups();
    if (setups.empty()) {
        return list;
    }
    const auto layouts = PrecompileLayouts::Rank(precompile_records.layouts);
    list.render_setups = setups.size();
    list.attribute_layouts = layouts.ranked.size();
    // NVIDIA's driver reuses a compiled pipeline for the same vertex and pixel programs and render
    // targets, so the game's pairs are built for each render setup. A pair is recorded by what
    // shapes its pipeline: its programs, the render setup, the shader state and the attribute
    // layout, not the rest of the setup's key.
    for (const PrecompileSetup* setup : setups) {
        std::map<AttributeLayout, u32> setup_keys; ///< Index in base_keys by layout
        for (const auto& [vertex, pixel] : precompile_set.pairs) {
            for (const auto& layout : layouts.For(info.reads[vertex])) {
                ++list.layout_variants;
                if (hashes[vertex] == 0 || hashes[pixel] == 0) {
                    continue;
                }
                const std::array<u64, 4> parts{hashes[vertex], hashes[pixel], setup->render_setup,
                                               setup->shader_state};
                const u64 done = Common::CityHash64WithSeed(
                    reinterpret_cast<const char*>(layout.data()), layout.size(),
                    Common::CityHash64(reinterpret_cast<const char*>(parts.data()), sizeof(parts)));
                if (precompile_done.contains(done) || run_log.Skipped(done)) {
                    continue;
                }
                const auto [it, added] =
                    setup_keys.try_emplace(layout, static_cast<u32>(list.base_keys.size()));
                if (added) {
                    GraphicsPipelineCacheKey key = setup->key;
                    SetVertexTypes(key.state, layout);
                    key.state.alpha_test_func.Assign(
                        FixedPipelineState::PackComparisonOp(Maxwell::ComparisonOp::Always_GL));
                    key.state.xfb_enabled.Assign(0);
                    list.base_keys.push_back(key);
                }
                list.jobs.push_back(
                    {static_cast<u32>(vertex), static_cast<u32>(pixel), it->second, {}, done});
            }
        }
    }
    return list;
}

PrecompileBuildCounts PipelineCache::BuildPrecompileJobs(
    const PrecompileJobList& list, const PrecompileProgramInfo& info, PrecompileRunLog& run_log,
    const VideoCore::DiskResourceLoadCallback& callback, std::stop_token stop_loading) {
    std::mutex progress_mutex;
    size_t finished = 0;
    std::atomic<size_t> built{};
    std::atomic<size_t> rejected{};
    std::atomic<size_t> failed{};
    // Records go to disk only after the Vulkan cache file holds the pipelines they name, so a
    // precompile stopped by a crash or kill keeps what it built without claiming more.
    const bool can_checkpoint =
        !use_vulkan_pipeline_cache || device.CanReadPipelineCacheDuringBuilds();
    std::mutex checkpoint_mutex;
    auto last_checkpoint = std::chrono::steady_clock::now();
    std::chrono::seconds checkpoint_interval = PrecompileCheckpointInterval(0);
    const auto checkpoint = [&] {
        std::unique_lock lock{checkpoint_mutex, std::try_to_lock};
        if (!lock || std::chrono::steady_clock::now() - last_checkpoint < checkpoint_interval) {
            return;
        }
        std::unordered_set<u64> done;
        {
            std::scoped_lock done_lock{precompile_mutex};
            done = precompile_done;
        }
        if (use_vulkan_pipeline_cache) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         PipelineCacheVersion());
            size_t size = 0;
            vulkan_pipeline_cache.Read(&size, nullptr);
            checkpoint_interval = PrecompileCheckpointInterval(size);
        }
        SavePrecompiled(done);
        last_checkpoint = std::chrono::steady_clock::now();
    };
    callback(VideoCore::LoadCallbackStage::Build, 0, list.jobs.size());
    for (const PrecompileJob& job : list.jobs) {
        precompile_workers->QueueWork([&, job] {
            if (stop_loading.stop_requested()) {
                return;
            }
            const bool compute = job.compute_key.unique_hash != 0;
            // Built on this thread, which builds nothing at runtime.
            skip_pipeline_build_reports = true;
            run_log.Start(job.done);
            const PrecompileResult result =
                compute ? PrecompileCompute(job.vertex, job.compute_key)
                        : PrecompilePipeline(job.vertex, job.pixel, list.Key(job, info.hashes));
            if (result == PrecompileResult::Failed) {
                // Left unfinished in the run log: tried once more, then skipped.
                failed.fetch_add(1, std::memory_order_relaxed);
            } else {
                run_log.End(job.done);
                // Rejected pipelines are recorded too: they fail the same way until the
                // emulator build or the driver changes.
                (result == PrecompileResult::Built ? built : rejected)
                    .fetch_add(1, std::memory_order_relaxed);
                {
                    std::scoped_lock lock{precompile_mutex};
                    precompile_done.insert(job.done);
                }
                if (can_checkpoint) {
                    checkpoint();
                }
            }
            std::scoped_lock lock{progress_mutex};
            callback(VideoCore::LoadCallbackStage::Build, ++finished, list.jobs.size());
        });
    }
    precompile_workers->WaitForRequests(stop_loading);
    return {built.load(), rejected.load(), failed.load()};
}

PipelineCache::PrecompileResult PipelineCache::PrecompilePipeline(
    size_t vertex_index, size_t pixel_index, const GraphicsPipelineCacheKey& key) {
    const auto& programs = precompile_set.programs;
    auto vertex = MakeScannedEnvironment(programs[vertex_index], Shader::Stage::VertexB);
    auto fragment = MakeScannedEnvironment(programs[pixel_index], Shader::Stage::Fragment);
    if (!vertex || !fragment) {
        return PrecompileResult::Rejected;
    }
    const std::array<Shader::Environment*, 2> envs{&*vertex, &*fragment};
    try {
        ShaderPools pools;
        // Built on this thread: the pipeline is complete, or its creation failed, on return.
        // No pipeline means the recompiler or the device limits rejected it; recompiler errors
        // are caught and logged there.
        const auto pipeline = CreateGraphicsPipeline(pools, key, envs, nullptr, false);
        if (!pipeline) {
            return PrecompileResult::Rejected;
        }
        return pipeline->HasPipeline() ? PrecompileResult::Built : PrecompileResult::Failed;
    } catch (const std::exception& exception) {
        LOG_DEBUG(Render_Vulkan, "Precompile of programs {} and {} failed: {}", vertex_index,
                  pixel_index, exception.what());
        return PrecompileResult::Failed;
    }
}

PipelineCache::PrecompileResult PipelineCache::PrecompileCompute(
    size_t program, const ComputePipelineCacheKey& key) {
    auto env = MakeScannedEnvironment(precompile_set.programs[program], Shader::Stage::Compute);
    if (!env) {
        return PrecompileResult::Rejected;
    }
    env->SetComputeDispatch(key.workgroup_size, key.shared_memory_size);
    try {
        ShaderPools pools;
        const auto entry = CreateComputePipeline(pools, key, *env, nullptr, false);
        if (!entry.pipeline) {
            return PrecompileResult::Rejected;
        }
        return entry.pipeline->HasPipeline() ? PrecompileResult::Built : PrecompileResult::Failed;
    } catch (const std::exception& exception) {
        LOG_DEBUG(Render_Vulkan, "Precompile of compute program {} failed: {}", program,
                  exception.what());
        return PrecompileResult::Failed;
    }
}

// The precompile's files name the cache version, the emulator build (the recompiler's output can
// change while program hashes stay), the device and driver and the dynamic state in use (set by
// the dyna_state and vertex_input_dynamic_state settings) they were written with; a change of
// any, or a lost vulkan_pipelines.bin, redoes the precompile. A driver cache that was cleared, or
// that dropped entries to stay within its size limit, goes unnoticed: those pipelines are then
// built on first use, as without the precompile. A full Mario Kart World precompile adds 641 MB
// to NVIDIA's; __GL_SHADER_DISK_CACHE_SIZE and MESA_SHADER_CACHE_MAX_SIZE set the limits.
PrecompileFileIdentity PipelineCache::PrecompileIdentity() const {
    struct {
        std::array<u8, VK_UUID_SIZE> uuid;
        u32 driver_id;
        u32 driver_version;
        DynamicFeatures features;
    } identity{};
    static_assert(std::has_unique_object_representations_v<decltype(identity)>);
    std::ranges::copy(device.GetPipelineCacheUUID(), identity.uuid.begin());
    identity.driver_id = static_cast<u32>(device.GetDriverID());
    identity.driver_version = device.GetDriverVersion();
    identity.features = dynamic_features;
    const std::string_view build{Common::g_scm_rev};
    return {PipelineCacheVersion(),
            Common::CityHash64WithSeed(reinterpret_cast<const char*>(&identity), sizeof(identity),
                                       Common::CityHash64(build.data(), build.size()))};
}

std::filesystem::path PipelineCache::PrecompileFile(std::string_view name) const {
    if (pipeline_cache_filename.empty()) {
        return {};
    }
    return pipeline_cache_filename.parent_path() / name;
}

void PipelineCache::LoadPrecompiled() {
    const auto path = PrecompileFile("precompiled.bin");
    // Pipelines precompiled with neither cache kept are gone with the process.
    if (path.empty() || (use_vulkan_pipeline_cache ? !vulkan_pipeline_cache_loaded
                                                   : !DriverKeepsPipelines(device))) {
        return;
    }
    precompile_done.merge(ReadPrecompiledRecords(path, PrecompileIdentity()));
}

void PipelineCache::SavePrecompiled(const std::unordered_set<u64>& done) {
    const auto path = PrecompileFile("precompiled.bin");
    if (!path.empty() && !done.empty()) {
        WritePrecompiledRecords(path, PrecompileIdentity(), done);
    }
}

void PipelineCache::StopPrecompile() {
    precompile_collector.request_stop();
    if (precompile_collector.joinable()) {
        precompile_collector.join();
    }
    // Joins the threads: a build in progress finishes, queued ones are dropped.
    precompile_workers.reset();
    precompile_set = {};
}

} // namespace Vulkan
