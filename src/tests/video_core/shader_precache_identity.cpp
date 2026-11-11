// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Whether the shader precache builds the SPIR-V the game's dispatches build. Hidden:
//   EGLANTINE_WARM_IDENTITY=<shader dir>/<title id> ./tests "[.warm_identity]"
// Pairs the compute entries of warm-compute.bin with those of vulkan.bin (what the game ran) by
// pipeline key, translates both with one profile, the precache entry with the guesses the
// precache uses, and compares the SPIR-V. A pair that differs gave the driver a program the
// dispatch won't ask for, so the dispatch still compiles. EGLANTINE_WARM_IDENTITY_OUT=<file>
// writes one line per pair.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/cityhash.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/stage_cache.h"

namespace {

using Key = Vulkan::ComputePipelineCacheKey;

struct KeyLess {
    bool operator()(const Key& lhs, const Key& rhs) const {
        return std::memcmp(&lhs, &rhs, sizeof(Key)) < 0;
    }
};
using ComputeEntries = std::map<Key, VideoCommon::FileEnvironment, KeyLess>;

std::string Translate(Shader::Environment& env) try {
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::HostTranslateInfo host{};
    host.support_float16 = true;
    host.support_int64 = true;
    host.support_float64 = true;
    host.min_ssbo_alignment = 16;
    host.support_viewport_index_layer = true;
    host.support_conditional_barrier = true;
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_int16 = true;
    profile.support_int8 = true;
    profile.support_descriptor_aliasing = true;
    profile.support_explicit_workgroup_layout = true;
    profile.support_vote = true;
    profile.warp_size_potentially_larger_than_guest = false;

    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, env.StartAddress()};
    Shader::IR::Program program{
        Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host)};
    const std::vector<u32> spirv{Shader::Backend::SPIRV::EmitSPIRV(profile, program)};
    return fmt::format("{:016x}", Common::CityHash64(reinterpret_cast<const char*>(spirv.data()),
                                                     spirv.size() * sizeof(u32)));
} catch (const std::exception& exception) {
    return fmt::format("error {}", std::string{exception.what()}.substr(0, 100));
}

/// The reads a game translation made that the precache answers differently, by kind (the
/// stage cache's read kinds). Only the first differing read counts: later ones may sit on a path
/// the precache never takes.
std::string FirstGuessDifference(std::span<const VideoCommon::StageCache::Translation::Read> reads,
                                 Shader::Environment& warm, std::string& detail) {
    for (const auto& read : reads) {
        const char* bank = read.a == 0 ? "bank 0" : "other banks";
        u64 guessed{};
        std::string what;
        switch (read.kind) {
        case 0:
            guessed = warm.ReadCbufValue(read.a, read.b);
            what = fmt::format("constant ({})", bank);
            break;
        case 1:
            guessed = warm.IsCbufStorageBufferPointer(read.a, read.b);
            what = fmt::format("storage pointer ({})", bank);
            break;
        case 2:
            guessed = warm.IsCbufIndirectStorageBufferPointer(read.a, read.b, read.c);
            what = "indirect storage pointer";
            break;
        case 3:
            guessed = static_cast<u64>(warm.ReadTextureType(read.a));
            what = "texture type";
            break;
        case 4:
            guessed = static_cast<u64>(warm.ReadTexturePixelFormat(read.a));
            what = "texture pixel format";
            break;
        case 5:
            guessed = warm.IsTexturePixelFormatInteger(read.a);
            what = "texture integer format";
            break;
        default:
            continue;
        }
        if (guessed != read.value) {
            detail = fmt::format("{}/{:#x}/{:#x}: game {:#x}, guess {:#x}", read.a, read.b, read.c,
                                 read.value, guessed);
            return what;
        }
    }
    return {};
}

/// Reads the compute entries of a pipeline cache file (vulkan.bin or warm-compute.bin) without
/// the loader, which deletes a file it rejects.
ComputeEntries ReadCompute(const std::filesystem::path& path) {
    ComputeEntries out;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return out;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(8 + sizeof(u32));
    try {
        while (file.tellg() != end) {
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            std::vector<VideoCommon::FileEnvironment> envs(num_envs);
            for (auto& env : envs) {
                env.Deserialize(file);
            }
            if (envs.size() == 1 && envs.front().ShaderStage() == Shader::Stage::Compute) {
                Key key;
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
                // A program rebuilt for a filled pointer slot has a later record: keep the last.
                out.insert_or_assign(key, std::move(envs.front()));
            } else {
                Vulkan::GraphicsPipelineCacheKey key;
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
            }
        }
    } catch (const std::ios_base::failure&) {
        std::printf("warm identity: %s ends early\n", path.string().c_str());
    }
    return out;
}

} // Anonymous namespace

TEST_CASE("Shader precache builds the game's SPIR-V", "[.warm_identity]") {
    const char* const directory = std::getenv("EGLANTINE_WARM_IDENTITY");
    if (directory == nullptr) {
        SKIP("EGLANTINE_WARM_IDENTITY is not set");
    }
    const std::filesystem::path dir{directory};
    auto runtime = ReadCompute(dir / "vulkan.bin");
    auto warm = ReadCompute(dir / "warm-compute.bin");
    REQUIRE(!runtime.empty());
    REQUIRE(!warm.empty());

    static const VideoCommon::GuessMissing guess{};
    size_t pairs{}, identical{}, differ{}, warm_errors{}, runtime_errors{};
    std::map<std::string, size_t> field_differences;
    std::map<std::string, size_t> causes;
    std::FILE* out = nullptr;
    if (const char* const path = std::getenv("EGLANTINE_WARM_IDENTITY_OUT")) {
        out = std::fopen(path, "w");
    }
    for (auto& [key, game_env] : runtime) {
        const auto it = warm.find(key);
        if (it == warm.end()) {
            continue;
        }
        ++pairs;
        auto& warm_env = it->second;
        std::string fields;
        const auto note = [&](const char* name, auto a, auto b) {
            if (a != b) {
                fields += fmt::format(" {}={}/{}", name, a, b);
                ++field_differences[name];
            }
        };
        note("local", game_env.LocalMemorySize(), warm_env.LocalMemorySize());
        note("texbank", game_env.TextureBoundBuffer(), warm_env.TextureBoundBuffer());
        note("start", game_env.StartAddress(), warm_env.StartAddress());
        note("shared", game_env.SharedMemorySize(), warm_env.SharedMemorySize());

        warm_env.SetGuessMissing(&guess);
        VideoCommon::StageCache::Recorder recorder{game_env};
        const std::string game_spirv = Translate(recorder);
        const std::string warm_spirv = Translate(warm_env);
        const bool game_ok = !game_spirv.starts_with("error");
        const bool warm_ok = !warm_spirv.starts_with("error");
        runtime_errors += game_ok ? 0 : 1;
        warm_errors += warm_ok ? 0 : 1;
        const bool same = game_ok && warm_ok && game_spirv == warm_spirv;
        identical += same ? 1 : 0;
        differ += game_ok && warm_ok && !same ? 1 : 0;
        if (game_ok && warm_ok && !same) {
            const auto reads = recorder.TakeReads();
            std::string detail;
            std::string cause = FirstGuessDifference(reads, warm_env, detail);
            if (cause.empty()) {
                cause = "no read differs";
            }
            ++causes[cause];
            fields += fmt::format(" first wrong guess: {} {}", cause, detail);
        }
        if (out) {
            fmt::print(out, "{:016x} {} game {} warm {}{}\n", key.unique_hash,
                       same ? "same" : "DIFF", game_spirv, warm_spirv, fields);
        }
    }
    if (out) {
        std::fclose(out);
    }
    std::printf("warm identity: %zu game compute programs, %zu precache programs, %zu pairs\n",
                runtime.size(), warm.size(), pairs);
    std::printf("  identical SPIR-V %zu, different %zu, precache errors %zu, game errors %zu\n",
                identical, differ, warm_errors, runtime_errors);
    for (const auto& [name, count] : field_differences) {
        std::printf("  %s differs in %zu pairs\n", name.c_str(), count);
    }
    for (const auto& [cause, count] : causes) {
        std::printf("  different SPIR-V, first wrong guess %s: %zu\n", cause.c_str(), count);
    }
}
