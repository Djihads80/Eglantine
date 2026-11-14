// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace VideoCommon {

/// Reuses translated and emitted native shader stages across pipelines.
///
/// Pipelines that differ only in fixed state (blending, depth, render targets) translate the same
/// programs again: in Ocarina of Time 64% of the stages a cold boot translated gave SPIR-V the
/// same program had already given, half of the translation time. A stage's IR depends only on its
/// program, the environment's constant properties and the answers to the reads translation makes
/// (constant buffer values, texture types and formats, storage pointer checks, ...); its SPIR-V
/// also on the runtime info and the bindings the earlier stages took. A translation is reused when
/// those reads, replayed in the order translation made them, give the same answers: translation
/// is deterministic, so every read replayed up to a difference is one it would make too. Emitted
/// stages are reused when the runtime info and bindings are also byte-for-byte equal.
///
/// Thread safe: pipelines are built on the GPU thread, translation workers and boot workers.
class StageCache {
public:
    /// What a reused stage stands in for: the program's metadata, without its IR.
    struct Emitted {
        std::vector<u8> runtime_key; ///< Serialized runtime info and incoming bindings
        std::vector<u32> spirv;
        Shader::IR::Program program; ///< After emission, without IR
        Shader::Backend::Bindings bindings_out;
    };

    struct Translation {
        std::vector<u8> signature;
        struct Read {
            u8 kind;
            u32 a, b, c;
            u64 value;
        };
        std::vector<Read> reads;
        Shader::IR::Program program; ///< After translation, without IR

        mutable std::mutex emitted_mutex;
        mutable std::vector<std::shared_ptr<const Emitted>> emitted;
    };

    /// Records the reads translation makes through it, forwarding everything to `inner`.
    class Recorder final : public Shader::Environment {
    public:
        explicit Recorder(Shader::Environment& inner);

        u64 ReadInstruction(u32 address) override;
        u32 TranslateProgramAddress(u32 address) const override;
        bool IsNativeProgram() const override;
        bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;
        bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override;
        u32 ReadCbufValue(u32 index, u32 offset) override;
        Shader::TextureType ReadTextureType(u32 handle) override;
        Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;
        bool IsTexturePixelFormatInteger(u32 handle) override;
        u32 ReadViewportTransformState() override;
        u32 TextureBoundBuffer() const override;
        u32 LocalMemorySize() const override;
        u32 SharedMemorySize() const override;
        std::array<u32, 3> WorkgroupSize() const override;
        bool HasHLEMacroState() const override;
        std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;
        void Dump(u64 pipeline_hash, u64 shader_hash) override;

        std::vector<Translation::Read> TakeReads() {
            return std::move(reads);
        }

    private:
        u64 Log(u8 kind, u32 a, u32 b, u32 c, u64 value);

        Shader::Environment& inner;
        std::vector<Translation::Read> reads;
        std::unordered_set<u64> logged;
    };

    StageCache();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// Whether EDEN_NX2_STAGE_CACHE=verify: every reused stage is also built and compared.
    [[nodiscard]] bool Verify() const noexcept {
        return verify;
    }

    /// The bytes translation depends on besides the reads: program, slot, environment constants and
    /// the settings the recompiler reads.
    [[nodiscard]] static std::vector<u8> Signature(Shader::Environment& env, u64 unique_hash,
                                                   size_t slot);

    /// A translation of `signature` whose reads `env` answers the same way, or null.
    [[nodiscard]] std::shared_ptr<const Translation> Find(const std::vector<u8>& signature,
                                                          Shader::Environment& env);

    /// The emitted stage of `translation` for `runtime_key`, or null.
    [[nodiscard]] std::shared_ptr<const Emitted> FindEmitted(const Translation& translation,
                                                             const std::vector<u8>& runtime_key);

    /// Makes a translation (from a Recorder) findable, with its first emitted stage.
    void Insert(std::shared_ptr<Translation> translation, std::shared_ptr<const Emitted> emitted);

    /// Adds an emitted stage to a translation found earlier.
    void AddEmitted(const Translation& translation, std::shared_ptr<const Emitted> emitted);

    [[nodiscard]] static std::vector<u8> RuntimeKey(const Shader::RuntimeInfo& info,
                                                    const Shader::Backend::Bindings& bindings);

    /// Program metadata without its IR.
    [[nodiscard]] static Shader::IR::Program Summary(const Shader::IR::Program& program);

    /// Whether two programs have the same metadata (what Summary keeps).
    [[nodiscard]] static bool SameSummary(const Shader::IR::Program& a,
                                          const Shader::IR::Program& b);

    /// Counts for the periodic log line.
    void CountReuse(bool emitted);
    void CountTranslated();
    void CountVerifyMismatch(u64 unique_hash);

private:
    void Account(size_t bytes);
    void Log();

    bool enabled{};
    bool verify{};

    std::mutex mutex;
    std::unordered_map<u64, std::vector<std::shared_ptr<const Translation>>> translations;
    size_t bytes{};

    std::atomic<u64> reused_emitted{};
    std::atomic<u64> reused_translation{};
    std::atomic<u64> translated{};
    std::atomic<u64> verify_mismatches{};
    std::atomic<s64> last_log{};
};

} // namespace VideoCommon
