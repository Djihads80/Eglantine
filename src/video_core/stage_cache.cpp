// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "common/cityhash.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "video_core/stage_cache.h"

namespace VideoCommon {
namespace {

enum ReadKind : u8 {
    CbufValue,
    StoragePointer,
    IndirectStoragePointer,
    TextureType,
    TexturePixelFormat,
    TextureInteger,
    ViewportTransform,
    Replacement,
};

// Past this, the cache starts over (translations hold program metadata and SPIR-V).
constexpr size_t MaxBytes = 256ULL << 20;

class Writer {
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void Put(const T& value) {
        const auto* const begin = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), begin, begin + sizeof(T));
    }

    std::vector<u8> bytes;
};

u64 EncodeReplacement(const std::optional<Shader::ReplaceConstant>& value) {
    return value ? (u64{1} << 32) | static_cast<u32>(*value) : 0;
}

} // Anonymous namespace

StageCache::Recorder::Recorder(Shader::Environment& inner_) : inner{inner_} {
    sph = inner.SPH();
    gp_passthrough_mask = inner.GpPassthroughMask();
    mesh_registers = inner.MeshRegisters();
    mesh_primitive_header = inner.MeshPrimitiveHeader();
    stage = inner.ShaderStage();
    start_address = inner.StartAddress();
    is_proprietary_driver = inner.IsProprietaryDriver();
}

u64 StageCache::Recorder::Log(u8 kind, u32 a, u32 b, u32 c, u64 value) {
    // Only first reads: within one translation a repeated read gives the same answer.
    const u64 id = Common::CityHash64(reinterpret_cast<const char*>(std::array<u32, 4>{kind, a, b, c}.data()),
                                      sizeof(u32) * 4);
    if (logged.insert(id).second) {
        reads.push_back({kind, a, b, c, value});
    }
    return value;
}

u64 StageCache::Recorder::ReadInstruction(u32 address) {
    return inner.ReadInstruction(address);
}

u32 StageCache::Recorder::TranslateProgramAddress(u32 address) const {
    return inner.TranslateProgramAddress(address);
}

bool StageCache::Recorder::IsNativeProgram() const {
    return inner.IsNativeProgram();
}

bool StageCache::Recorder::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    return Log(StoragePointer, index, offset, 0, inner.IsCbufStorageBufferPointer(index, offset)) != 0;
}

bool StageCache::Recorder::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) {
    return Log(IndirectStoragePointer, index, offset, indirect,
               inner.IsCbufIndirectStorageBufferPointer(index, offset, indirect)) != 0;
}

u32 StageCache::Recorder::ReadCbufValue(u32 index, u32 offset) {
    return static_cast<u32>(Log(CbufValue, index, offset, 0, inner.ReadCbufValue(index, offset)));
}

Shader::TextureType StageCache::Recorder::ReadTextureType(u32 handle) {
    return static_cast<Shader::TextureType>(
        Log(TextureType, handle, 0, 0, static_cast<u64>(inner.ReadTextureType(handle))));
}

Shader::TexturePixelFormat StageCache::Recorder::ReadTexturePixelFormat(u32 handle) {
    return static_cast<Shader::TexturePixelFormat>(
        Log(TexturePixelFormat, handle, 0, 0, static_cast<u64>(inner.ReadTexturePixelFormat(handle))));
}

bool StageCache::Recorder::IsTexturePixelFormatInteger(u32 handle) {
    return Log(TextureInteger, handle, 0, 0, inner.IsTexturePixelFormatInteger(handle)) != 0;
}

u32 StageCache::Recorder::ReadViewportTransformState() {
    return static_cast<u32>(Log(ViewportTransform, 0, 0, 0, inner.ReadViewportTransformState()));
}

u32 StageCache::Recorder::TextureBoundBuffer() const {
    return inner.TextureBoundBuffer();
}

u32 StageCache::Recorder::LocalMemorySize() const {
    return inner.LocalMemorySize();
}

u32 StageCache::Recorder::SharedMemorySize() const {
    return inner.SharedMemorySize();
}

std::array<u32, 3> StageCache::Recorder::WorkgroupSize() const {
    return inner.WorkgroupSize();
}

bool StageCache::Recorder::HasHLEMacroState() const {
    return inner.HasHLEMacroState();
}

std::optional<Shader::ReplaceConstant> StageCache::Recorder::GetReplaceConstBuffer(u32 bank,
                                                                                   u32 offset) {
    const auto value = inner.GetReplaceConstBuffer(bank, offset);
    Log(Replacement, bank, offset, 0, EncodeReplacement(value));
    return value;
}

void StageCache::Recorder::Dump(u64 pipeline_hash, u64 shader_hash) {
    inner.Dump(pipeline_hash, shader_hash);
}

StageCache::StageCache() {
    // EDEN_NX2_STAGE_CACHE=0 translates every stage again; =verify also builds every reused stage
    // and compares the SPIR-V.
    const char* const value = std::getenv("EDEN_NX2_STAGE_CACHE");
    enabled = value == nullptr || value[0] != '0';
    verify = enabled && value != nullptr && std::string_view{value} == "verify";
    if (verify) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_STAGE_CACHE=verify: reused stages are rebuilt and compared");
    }
}

std::vector<u8> StageCache::Signature(Shader::Environment& env, u64 unique_hash, size_t slot) {
    Writer writer;
    writer.Put(unique_hash);
    writer.Put(static_cast<u32>(slot));
    writer.Put(env.ShaderStage());
    writer.Put(env.StartAddress());
    writer.Put(env.IsProprietaryDriver());
    writer.Put(env.IsNativeProgram());
    writer.Put(env.SPH());
    writer.Put(env.GpPassthroughMask());
    writer.Put(env.MeshRegisters());
    writer.Put(env.MeshPrimitiveHeader());
    writer.Put(env.TextureBoundBuffer());
    writer.Put(env.LocalMemorySize());
    writer.Put(env.SharedMemorySize());
    writer.Put(env.WorkgroupSize());
    writer.Put(env.HasHLEMacroState());
    // What the recompiler reads from the settings, which can change during a session.
    const auto& resolution = Settings::values.resolution_info;
    writer.Put(resolution.up_scale);
    writer.Put(resolution.down_shift);
    writer.Put(resolution.up_factor);
    writer.Put(resolution.down_factor);
    writer.Put(resolution.active);
    writer.Put(Settings::values.renderer_debug.GetValue());
    writer.Put(Settings::values.disable_shader_loop_safety_checks.GetValue());
    writer.Put(Settings::values.fix_bloom_effects.GetValue());
    return std::move(writer.bytes);
}

std::shared_ptr<const StageCache::Translation> StageCache::Find(const std::vector<u8>& signature,
                                                                Shader::Environment& env) {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(signature.data()),
                                        signature.size());
    std::vector<std::shared_ptr<const Translation>> candidates;
    {
        std::scoped_lock lock{mutex};
        const auto it = translations.find(hash);
        if (it == translations.end()) {
            return nullptr;
        }
        candidates = it->second;
    }
    for (const auto& candidate : candidates) {
        if (candidate->signature != signature) {
            continue;
        }
        bool same = true;
        try {
            // In the order translation read them: up to the first difference, every read is one
            // translating here would make too.
            for (const Translation::Read& read : candidate->reads) {
                u64 value{};
                switch (read.kind) {
                case CbufValue:
                    value = env.ReadCbufValue(read.a, read.b);
                    break;
                case StoragePointer:
                    value = env.IsCbufStorageBufferPointer(read.a, read.b);
                    break;
                case IndirectStoragePointer:
                    value = env.IsCbufIndirectStorageBufferPointer(read.a, read.b, read.c);
                    break;
                case TextureType:
                    value = static_cast<u64>(env.ReadTextureType(read.a));
                    break;
                case TexturePixelFormat:
                    value = static_cast<u64>(env.ReadTexturePixelFormat(read.a));
                    break;
                case TextureInteger:
                    value = env.IsTexturePixelFormatInteger(read.a);
                    break;
                case ViewportTransform:
                    value = env.ReadViewportTransformState();
                    break;
                case Replacement:
                    value = EncodeReplacement(env.GetReplaceConstBuffer(read.a, read.b));
                    break;
                }
                if (value != read.value) {
                    same = false;
                    break;
                }
            }
        } catch (const Shader::Exception&) {
            // A disk cache environment that never recorded this read.
            same = false;
        }
        if (same) {
            return candidate;
        }
    }
    return nullptr;
}

std::shared_ptr<const StageCache::Emitted> StageCache::FindEmitted(
    const Translation& translation, const std::vector<u8>& runtime_key) {
    std::scoped_lock lock{translation.emitted_mutex};
    for (const auto& emitted : translation.emitted) {
        if (emitted->runtime_key == runtime_key) {
            return emitted;
        }
    }
    return nullptr;
}

void StageCache::Insert(std::shared_ptr<Translation> translation,
                        std::shared_ptr<const Emitted> emitted) {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(translation->signature.data()),
                                        translation->signature.size());
    const size_t size = translation->reads.size() * sizeof(Translation::Read) +
                        translation->signature.size() + sizeof(Translation) +
                        emitted->spirv.size() * sizeof(u32) + sizeof(Emitted);
    translation->emitted.push_back(std::move(emitted));
    {
        std::scoped_lock lock{mutex};
        translations[hash].push_back(std::move(translation));
    }
    Account(size);
    CountTranslated();
}

void StageCache::AddEmitted(const Translation& translation, std::shared_ptr<const Emitted> emitted) {
    const size_t size = emitted->spirv.size() * sizeof(u32) + sizeof(Emitted);
    {
        std::scoped_lock lock{translation.emitted_mutex};
        translation.emitted.push_back(std::move(emitted));
    }
    Account(size);
}

void StageCache::Account(size_t added) {
    std::scoped_lock lock{mutex};
    bytes += added;
    if (bytes > MaxBytes) {
        // Translations in use elsewhere stay alive through their shared pointers.
        translations.clear();
        bytes = 0;
    }
}

std::vector<u8> StageCache::RuntimeKey(const Shader::RuntimeInfo& info,
                                       const Shader::Backend::Bindings& bindings) {
    // Every field of RuntimeInfo: one left out would let different SPIR-V share a key.
    Writer writer;
    writer.Put(bindings);
    writer.Put(info.generic_input_types);
    for (size_t index = 0; index < info.previous_stage_stores.mask.size(); index += 64) {
        u64 word{};
        for (size_t bit = 0; bit < 64; ++bit) {
            word |= u64{info.previous_stage_stores.mask[index + bit]} << bit;
        }
        writer.Put(word);
    }
    writer.Put(static_cast<u32>(info.per_primitive_generics.to_ulong()));
    writer.Put(static_cast<u32>(info.previous_stage_legacy_stores_mapping.size()));
    for (const auto& [from, to] : info.previous_stage_legacy_stores_mapping) {
        writer.Put(from);
        writer.Put(to);
    }
    writer.Put(info.convert_depth_mode);
    writer.Put(info.force_early_z);
    writer.Put(info.tess_primitive);
    writer.Put(info.tess_spacing);
    writer.Put(info.tess_clockwise);
    writer.Put(info.input_topology);
    writer.Put(info.fixed_state_point_size.has_value());
    writer.Put(info.fixed_state_point_size.value_or(0.0f));
    writer.Put(info.alpha_test_func.has_value());
    writer.Put(info.alpha_test_func.value_or(Shader::CompareFunction{}));
    writer.Put(info.alpha_test_reference);
    writer.Put(info.y_negate);
    writer.Put(info.glasm_use_storage_buffers);
    writer.Put(info.xfb_count);
    for (u32 index = 0; index < info.xfb_count && index < info.xfb_varyings.size(); ++index) {
        const auto& varying = info.xfb_varyings[index];
        writer.Put(varying.buffer);
        writer.Put(varying.stream);
        writer.Put(varying.stride);
        writer.Put(varying.offset);
        writer.Put(varying.components);
    }
    writer.Put(info.color_output_types);
    writer.Put(info.dual_source_blend);
    return std::move(writer.bytes);
}

Shader::IR::Program StageCache::Summary(const Shader::IR::Program& program) {
    Shader::IR::Program summary;
    summary.info = program.info;
    summary.stage = program.stage;
    summary.workgroup_size = program.workgroup_size;
    summary.output_topology = program.output_topology;
    summary.output_vertices = program.output_vertices;
    summary.invocations = program.invocations;
    summary.local_memory_size = program.local_memory_size;
    summary.shared_memory_size = program.shared_memory_size;
    summary.is_geometry_passthrough = program.is_geometry_passthrough;
    summary.mesh = program.mesh;
    return summary;
}

bool StageCache::SameSummary(const Shader::IR::Program& a, const Shader::IR::Program& b) {
    return a.info == b.info && a.stage == b.stage && a.workgroup_size == b.workgroup_size &&
           a.output_topology == b.output_topology && a.output_vertices == b.output_vertices &&
           a.invocations == b.invocations && a.local_memory_size == b.local_memory_size &&
           a.shared_memory_size == b.shared_memory_size &&
           a.is_geometry_passthrough == b.is_geometry_passthrough && a.mesh == b.mesh;
}

void StageCache::CountReuse(bool emitted) {
    (emitted ? reused_emitted : reused_translation).fetch_add(1, std::memory_order_relaxed);
    Log();
}

void StageCache::CountTranslated() {
    translated.fetch_add(1, std::memory_order_relaxed);
    Log();
}

void StageCache::CountVerifyMismatch(u64 unique_hash) {
    verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    LOG_ERROR(Render_Vulkan, "Stage cache: reused stage {:016x} differs from a fresh build",
              unique_hash);
}

void StageCache::Log() {
    const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = last_log.load(std::memory_order_relaxed);
    if (now - last < 10 || !last_log.compare_exchange_strong(last, now)) {
        return;
    }
    if (last == 0) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Stage cache: {} stages reused whole, {} translations reused, {} translated{}",
             reused_emitted.load(), reused_translation.load(), translated.load(),
             verify ? fmt::format(", {} verify mismatches", verify_mismatches.load())
                    : std::string{});
}

} // namespace VideoCommon
