// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Reuse of translated shaders between graphics pipelines (NX2_SHADER_STUTTER.md, "Shader reuse").
//
// Games create many graphics pipelines that run the same programs and differ only in fixed
// function state: blending, depth, vertex formats, render targets... Each one used to translate
// its programs and emit SPIR-V again, milliseconds per pipeline. That work reads:
// - the programs, the few pipeline key bits MakeRuntimeInfo turns into Shader::RuntimeInfo, and
//   some settings: together, the ShaderTranslationKey;
// - the answers of the programs' environments (constant buffer words, texture types, ...). They
//   come from live GPU state and are in no key, so TranslationReads records them, and a pipeline
//   only reuses shaders when its own environments give the same answers.
// Translation is deterministic: with the same inputs it would build the same SPIR-V again.

#include <array>
#include <compare>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/transform_feedback.h"

namespace Vulkan {

/// Everything besides the environments that translation and SPIR-V emission of a graphics
/// pipeline read (MakeShaderTranslationKey in vk_pipeline_cache.cpp). Fields the pipeline's
/// stages do not read stay zero, so they do not keep pipelines from sharing shaders.
struct ShaderTranslationKey {
    std::array<u64, Maxwell::MaxShaderProgram> unique_hashes;

    /// Transform feedback layout, when enabled and supported.
    VideoCommon::TransformFeedbackState xfb_state;

    /// Vertex attribute types as the vertex shader sees them (not their formats or bindings).
    std::array<Shader::AttributeType, Maxwell::NumVertexAttributes> generic_input_types;
    /// Render target output types; only MoltenVK needs them, Float elsewhere.
    std::array<Shader::AttributeType, Maxwell::NumRenderTargets> color_output_types;

    /// The topology as the shaders see it: its class, and the output of a generated layer
    /// passthrough geometry stage.
    Shader::InputTopology input_topology;
    Shader::OutputTopology passthrough_topology;
    Shader::TessPrimitive tess_primitive;
    Shader::TessSpacing tess_spacing;
    Shader::CompareFunction alpha_test_func;
    u32 alpha_test_reference; ///< Float bits.
    u32 point_size;           ///< Float bits, only when a stage may write a fixed point size.

    /// Settings read by the translation passes and the SPIR-V backend.
    u32 resolution_up_scale;
    u32 resolution_down_shift;
    bool rescaling_active;
    bool renderer_debug;
    bool disable_shader_loop_safety_checks;
    bool fix_bloom_effects;

    bool xfb_enabled;
    bool convert_depth_mode;
    bool force_early_z;
    bool y_negate;
    bool tess_clockwise;
    bool dual_source_blend;
    /// Always zero: makes the padding explicit, so the key can be compared and hashed as bytes.
    std::array<u8, 2> zero_padding;

    bool operator==(const ShaderTranslationKey& rhs) const noexcept;
};
static_assert(std::has_unique_object_representations_v<ShaderTranslationKey>);
static_assert(std::is_trivially_copyable_v<ShaderTranslationKey>);

struct ShaderTranslationKeyHash {
    size_t operator()(const ShaderTranslationKey& key) const noexcept;
};

/// What translation asked one program's environment, and the answers.
class TranslationReads {
public:
    enum class Query : u32 {
        CbufValue,
        TextureType,
        TexturePixelFormat,
        TexturePixelFormatInteger,
        ViewportTransformState,
        ReplaceConstBuffer,
        HasHLEMacroState,
    };
    /// GetReplaceConstBuffer's answer when there is no replacement.
    static constexpr u32 NoReplacement = ~0U;

    /// Asks `env` a question; the answer as a word (the enums' values, 0/1 for booleans).
    static u32 Ask(Shader::Environment& env, Query query, u32 arg0, u32 arg1);

    /// Records the environment's fixed properties (stage, header, texture buffer...).
    void RecordProperties(const Shader::Environment& env);

    void Record(Query query, u32 arg0, u32 arg1, u32 answer);

    /// Sorts the reads and drops repeated ones: translation asks some words many times.
    void Finish();

    /// Whether `env` has the same properties and gives the same answer to every read. Its own
    /// read log (what the disk cache stores) gets the same entries a translation would add.
    [[nodiscard]] bool Matches(Shader::Environment& env) const;

    bool operator==(const TranslationReads&) const = default;

private:
    /// What translation reads of an environment without a request that could be recorded (the
    /// header, stage, ...), or that is the same for live and disk cache environments. The rest
    /// is recorded when asked: HLE macro state, for one, differs between the two.
    struct Properties {
        std::array<u32, sizeof(Shader::ProgramHeader) / sizeof(u32)> sph;
        std::array<u32, 8> gp_passthrough_mask; ///< Geometry programs only.
        std::array<u32, 3> workgroup_size;
        Shader::Stage stage;
        u32 start_address;
        u32 texture_bound_buffer;
        u32 local_memory_size;
        u32 shared_memory_size;
        bool is_proprietary_driver;

        bool operator==(const Properties&) const = default;
    };
    static Properties PropertiesOf(const Shader::Environment& env);

    struct Read {
        Query query;
        u32 arg0;
        u32 arg1;
        u32 answer;

        auto operator<=>(const Read&) const = default;
    };

    Properties properties{};
    std::vector<Read> reads;
};

/// Forwards every request to another environment, recording what translation asks it.
class RecordingEnvironment final : public Shader::Environment {
public:
    explicit RecordingEnvironment(Shader::Environment& inner_);

    u64 ReadInstruction(u32 address) override;
    u32 TranslateProgramAddress(u32 address) const override;
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 raw_handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) override;
    bool IsTexturePixelFormatInteger(u32 raw_handle) override;
    u32 ReadViewportTransformState() override;
    u32 TextureBoundBuffer() const override;
    u32 LocalMemorySize() const override;
    u32 SharedMemorySize() const override;
    std::array<u32, 3> WorkgroupSize() const override;
    bool HasHLEMacroState() const override;
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;
    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    /// The reads recorded so far, finished. Call once, after translation.
    [[nodiscard]] TranslationReads TakeReads();

private:
    u32 AskAndRecord(TranslationReads::Query query, u32 arg0, u32 arg1 = 0) const;

    Shader::Environment* inner;
    /// Mutable: HasHLEMacroState, a const request, is recorded too.
    mutable TranslationReads reads;
};

/// The shaders of a graphics pipeline, as built for the first key that needed them.
struct ReusableShaders {
    std::shared_ptr<const GraphicsShaderModules> modules;
    std::array<Shader::Info, Maxwell::MaxShaderStage> infos;
    /// What translation asked each program's environment, in the order the environments come.
    boost::container::static_vector<TranslationReads, Maxwell::MaxShaderProgram> reads;

    /// The stage infos as GraphicsPipeline takes them: null for stages without a module.
    [[nodiscard]] std::array<const Shader::Info*, Maxwell::MaxShaderStage> InfoPointers() const;

    /// Whether a pipeline with these environments would have been translated to these shaders.
    [[nodiscard]] bool FitEnvironments(std::span<Shader::Environment* const> envs) const;
};

/// Shaders built for earlier graphics pipelines, by translation key. Thread safe: the GPU thread,
/// the translation workers and the disk cache replay workers all use it.
class ShaderReuseCache {
public:
    struct Lookup {
        std::shared_ptr<const ReusableShaders> shaders; ///< Null when nothing fits.
        bool rejected{}; ///< Shaders with this key existed, but none fit the environments.
    };

    /// Shaders of an earlier pipeline with this key that fit `envs`.
    [[nodiscard]] Lookup Find(const ShaderTranslationKey& key,
                              std::span<Shader::Environment* const> envs) const;

    /// Whether shaders with this key exist (they may still not fit the environments).
    [[nodiscard]] bool Contains(const ShaderTranslationKey& key) const;

    void Add(const ShaderTranslationKey& key, std::shared_ptr<const ReusableShaders> shaders);

private:
    /// Variants kept per key: same programs and state, different environment answers (say, a
    /// texture bound as 2D by one pipeline and as an array by another). Further ones are not
    /// kept, so a key whose answers keep changing does not grow without bound.
    static constexpr size_t MaxVariants = 4;
    using Variants = boost::container::small_vector<std::shared_ptr<const ReusableShaders>, 1>;

    mutable std::mutex mutex;
    ::Common::unordered_map<ShaderTranslationKey, Variants, ShaderTranslationKeyHash> entries;
};

} // namespace Vulkan
