// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/assert.h"
#include "common/cityhash.h"
#include "shader_recompiler/exception.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"

namespace Vulkan {

bool ShaderTranslationKey::operator==(const ShaderTranslationKey& rhs) const noexcept {
    return std::memcmp(this, &rhs, sizeof *this) == 0;
}

size_t ShaderTranslationKeyHash::operator()(const ShaderTranslationKey& key) const noexcept {
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(&key), sizeof key));
}

u32 TranslationReads::Ask(Shader::Environment& env, Query query, u32 arg0, u32 arg1) {
    switch (query) {
    case Query::CbufValue:
        return env.ReadCbufValue(arg0, arg1);
    case Query::TextureType:
        return static_cast<u32>(env.ReadTextureType(arg0));
    case Query::TexturePixelFormat:
        return static_cast<u32>(env.ReadTexturePixelFormat(arg0));
    case Query::TexturePixelFormatInteger:
        return env.IsTexturePixelFormatInteger(arg0) ? 1 : 0;
    case Query::ViewportTransformState:
        return env.ReadViewportTransformState();
    case Query::ReplaceConstBuffer: {
        const std::optional<Shader::ReplaceConstant> replacement{
            env.GetReplaceConstBuffer(arg0, arg1)};
        return replacement ? static_cast<u32>(*replacement) : NoReplacement;
    }
    case Query::HasHLEMacroState:
        return env.HasHLEMacroState() ? 1 : 0;
    }
    UNREACHABLE_MSG("Unknown environment query {}", static_cast<u32>(query));
}

TranslationReads::Properties TranslationReads::PropertiesOf(const Shader::Environment& env) {
    Properties result{};
    std::memcpy(result.sph.data(), &env.SPH(), sizeof(result.sph));
    // Only geometry programs read the passthrough mask, and only theirs is in the disk cache.
    if (env.ShaderStage() == Shader::Stage::Geometry) {
        result.gp_passthrough_mask = env.GpPassthroughMask();
    }
    result.workgroup_size = env.WorkgroupSize();
    result.stage = env.ShaderStage();
    result.start_address = env.StartAddress();
    result.texture_bound_buffer = env.TextureBoundBuffer();
    result.local_memory_size = env.LocalMemorySize();
    result.shared_memory_size = env.SharedMemorySize();
    result.is_proprietary_driver = env.IsProprietaryDriver();
    return result;
}

void TranslationReads::RecordProperties(const Shader::Environment& env) {
    properties = PropertiesOf(env);
}

void TranslationReads::Record(Query query, u32 arg0, u32 arg1, u32 answer) {
    reads.push_back({query, arg0, arg1, answer});
}

void TranslationReads::Finish() {
    std::ranges::sort(reads);
    const auto [first, last] = std::ranges::unique(reads);
    reads.erase(first, last);
    reads.shrink_to_fit();
}

bool TranslationReads::Matches(Shader::Environment& env) const try {
    if (PropertiesOf(env) != properties) {
        return false;
    }
    return std::ranges::all_of(reads, [&env](const Read& read) {
        return Ask(env, read.query, read.arg0, read.arg1) == read.answer;
    });
} catch (const Shader::Exception&) {
    // A disk cache environment throws when asked what its own translation did not ask.
    return false;
}

RecordingEnvironment::RecordingEnvironment(Shader::Environment& inner_) : inner{&inner_} {
    // Shader::Environment's own accessors read these members.
    sph = inner->SPH();
    gp_passthrough_mask = inner->GpPassthroughMask();
    stage = inner->ShaderStage();
    start_address = inner->StartAddress();
    is_proprietary_driver = inner->IsProprietaryDriver();
    reads.RecordProperties(*inner);
}

// Instructions and program addresses are not recorded: they are the program's code, which its
// unique hash already identifies.
u64 RecordingEnvironment::ReadInstruction(u32 address) {
    return inner->ReadInstruction(address);
}

u32 RecordingEnvironment::TranslateProgramAddress(u32 address) const {
    return inner->TranslateProgramAddress(address);
}

u32 RecordingEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    return AskAndRecord(TranslationReads::Query::CbufValue, cbuf_index, cbuf_offset);
}

Shader::TextureType RecordingEnvironment::ReadTextureType(u32 raw_handle) {
    return static_cast<Shader::TextureType>(
        AskAndRecord(TranslationReads::Query::TextureType, raw_handle));
}

Shader::TexturePixelFormat RecordingEnvironment::ReadTexturePixelFormat(u32 raw_handle) {
    return static_cast<Shader::TexturePixelFormat>(
        AskAndRecord(TranslationReads::Query::TexturePixelFormat, raw_handle));
}

bool RecordingEnvironment::IsTexturePixelFormatInteger(u32 raw_handle) {
    return AskAndRecord(TranslationReads::Query::TexturePixelFormatInteger, raw_handle) != 0;
}

u32 RecordingEnvironment::ReadViewportTransformState() {
    return AskAndRecord(TranslationReads::Query::ViewportTransformState, 0);
}

std::optional<Shader::ReplaceConstant> RecordingEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                   u32 offset) {
    const u32 answer{AskAndRecord(TranslationReads::Query::ReplaceConstBuffer, bank, offset)};
    if (answer == TranslationReads::NoReplacement) {
        return std::nullopt;
    }
    return static_cast<Shader::ReplaceConstant>(answer);
}

// Recorded although const: disk cache environments answer it differently from live ones.
bool RecordingEnvironment::HasHLEMacroState() const {
    return AskAndRecord(TranslationReads::Query::HasHLEMacroState, 0) != 0;
}

// Fixed properties, compared as a whole by TranslationReads::Matches.
u32 RecordingEnvironment::TextureBoundBuffer() const {
    return inner->TextureBoundBuffer();
}

u32 RecordingEnvironment::LocalMemorySize() const {
    return inner->LocalMemorySize();
}

u32 RecordingEnvironment::SharedMemorySize() const {
    return inner->SharedMemorySize();
}

std::array<u32, 3> RecordingEnvironment::WorkgroupSize() const {
    return inner->WorkgroupSize();
}

void RecordingEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    inner->Dump(pipeline_hash, shader_hash);
}

TranslationReads RecordingEnvironment::TakeReads() {
    reads.Finish();
    return std::move(reads);
}

u32 RecordingEnvironment::AskAndRecord(TranslationReads::Query query, u32 arg0,
                                       u32 arg1) const {
    const u32 answer{TranslationReads::Ask(*inner, query, arg0, arg1)};
    reads.Record(query, arg0, arg1, answer);
    return answer;
}

std::array<const Shader::Info*, Maxwell::MaxShaderStage> ReusableShaders::InfoPointers() const {
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> result{};
    for (size_t stage = 0; stage < result.size(); ++stage) {
        if ((*modules)[stage]) {
            result[stage] = &infos[stage];
        }
    }
    return result;
}

bool ReusableShaders::FitEnvironments(std::span<Shader::Environment* const> envs) const {
    if (envs.size() != reads.size()) {
        return false;
    }
    for (size_t index = 0; index < envs.size(); ++index) {
        if (!reads[index].Matches(*envs[index])) {
            return false;
        }
    }
    return true;
}

ShaderReuseCache::Lookup ShaderReuseCache::Find(const ShaderTranslationKey& key,
                                                std::span<Shader::Environment* const> envs) const {
    Variants variants;
    {
        std::scoped_lock lock{mutex};
        const auto it{entries.find(key)};
        if (it == entries.end()) {
            return {};
        }
        variants = it->second;
    }
    // Checked without the lock: asking a live environment reads guest memory.
    for (const std::shared_ptr<const ReusableShaders>& shaders : variants) {
        if (shaders->FitEnvironments(envs)) {
            return {.shaders = shaders};
        }
    }
    return {.rejected = true};
}

bool ShaderReuseCache::Contains(const ShaderTranslationKey& key) const {
    std::scoped_lock lock{mutex};
    return entries.contains(key);
}

void ShaderReuseCache::Add(const ShaderTranslationKey& key,
                           std::shared_ptr<const ReusableShaders> shaders) {
    std::scoped_lock lock{mutex};
    Variants& variants{entries[key]};
    if (variants.size() >= MaxVariants) {
        return;
    }
    // Workers replaying the disk cache can translate the same shaders at the same time.
    const bool known{std::ranges::any_of(variants, [&shaders](const auto& variant) {
        return variant->reads == shaders->reads;
    })};
    if (!known) {
        variants.push_back(std::move(shaders));
    }
}

} // namespace Vulkan
