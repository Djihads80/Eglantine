// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/unique_function.h"
#include "shader_recompiler/environment.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"

namespace Tegra {
class Memorymanager;
}

namespace Shader::Ampere {
struct Instruction;
struct LoweredProgram;
} // namespace Shader::Ampere

namespace VideoCommon {

/// The shader hash of lowered native code (frame dump "shaders", pipeline cache keys).
u64 NativeShaderHash(std::span<const u64> code);

/// The code an environment holds for a lowered native program: `header` (the shader header,
/// zero for compute), the instructions with a Maxwell scheduling word before every fourth word,
/// a zero word and the native address map. Applies the per-program workarounds to `lowered`
/// first. The runtime and the shader precache builder must lay programs out the same way.
std::vector<u64> LayOutLoweredNativeCode(std::span<const u64> header,
                                         std::span<const Shader::Ampere::Instruction> native,
                                         Shader::Ampere::LoweredProgram& lowered);

/// A native (Switch 2) program after capture and lowering to Maxwell code. The shader cache keeps
/// it so the environments built for the program's pipelines don't capture and lower it again.
struct LoweredNativeCode {
    u64 diagnostic_key{};
    /// Shader::Ampere::NativeProgramKey of the captured program (shader dump file names).
    u64 native_program_key{};
    /// Shader header, lowered instructions with Maxwell scheduling words, then the native
    /// address map (what the environment's `code` used to be filled with).
    std::vector<u64> code;
    /// Bytes of the native program that were read, header included.
    size_t source_size{};
    /// Constant buffer holding the texture handles if the program requires one, otherwise ~0U
    /// (the engine's register value is used).
    u32 texture_bound = ~0U;
};

class GenericEnvironment : public Shader::Environment {
public:
    explicit GenericEnvironment() = default;
    explicit GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_);

    ~GenericEnvironment() override;

    [[nodiscard]] u32 TextureBoundBuffer() const final;

    [[nodiscard]] u32 LocalMemorySize() const final;

    [[nodiscard]] u32 SharedMemorySize() const final;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final;

    [[nodiscard]] u64 ReadInstruction(u32 address) final;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const override;

    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const override;

    /// Answers the classification of these keys (see MappedStoragePointerKeys) as storage buffer
    /// pointers, whatever the slots hold now; called before the program is translated
    void KeepStoragePointers(std::span<const u64> keys);

    [[nodiscard]] std::optional<u64> Analyze();

    void SetCachedSize(size_t size_bytes);

    [[nodiscard]] size_t CachedSizeWords() const noexcept;

    [[nodiscard]] size_t CachedSizeBytes() const noexcept;

    [[nodiscard]] size_t ReadSizeBytes() const noexcept;

    [[nodiscard]] size_t SourceSizeBytes() const noexcept {
        return native_source_size;
    }

    [[nodiscard]] const std::shared_ptr<const LoweredNativeCode>& LoweredNative() const noexcept {
        return lowered_native;
    }

    [[nodiscard]] bool CanBeSerialized() const noexcept;

    [[nodiscard]] u64 CalculateHash() const;

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    /// with_mesh_state: also the mesh registers and pass-through header of task and mesh
    /// programs (only the mesh pipeline file carries them).
    void Serialize(std::ofstream& file, bool with_mesh_state = false) const;

    bool HasHLEMacroState() const override {
        return has_hle_engine_state;
    }

protected:
    std::optional<u64> TryFindSize();

    /// Adopts an already lowered native program, or captures and lowers it when there is none.
    void InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                              u32 source_header_size = 128, bool geometry = false);
    std::shared_ptr<const LoweredNativeCode> LowerNativeCode(u32 source_header_size,
                                                              bool geometry = false);
    size_t native_source_size{};
    /// Lower matrix multiplies too (KeplerCompute::EmulatesNativeTensorPrograms).
    bool native_tensor{};
    /// ... in the compact form (KeplerCompute::UsesCompactTensorPrograms).
    bool native_compact_tensor{};
    std::shared_ptr<const LoweredNativeCode> lowered_native;

    Tegra::Texture::TICEntry ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                             bool via_header_index, u32 raw);

    Tegra::MemoryManager* gpu_memory{};
    GPUVAddr program_base{};

    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types; ///< IsCbufStorageBufferPointer answers
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;

    u32 local_memory_size{};
    u32 texture_bound{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};

    u32 read_lowest = (std::numeric_limits<u32>::max)();
    u32 read_highest = 0;

    u32 cached_lowest = (std::numeric_limits<u32>::max)();
    u32 cached_highest = 0;
    u32 initial_offset = 0;

    u32 viewport_transform_state = 1;

    bool has_unbound_instructions = false;
    bool has_hle_engine_state = false;
};

class GraphicsEnvironment final : public GenericEnvironment {
public:
    explicit GraphicsEnvironment() = default;
    explicit GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                 Tegra::MemoryManager& gpu_memory_,
                                 Tegra::Engines::Maxwell3D::Regs::ShaderType program,
                                 GPUVAddr program_base_, u32 start_address_,
                                 std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~GraphicsEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;

    /// Copies the 3D engine state translation reads and stops using the engine, so the program
    /// can be translated on a worker thread while the GPU thread moves on. Returns false, leaving
    /// the environment unchanged, when the texture pool isn't one contiguous device range or the
    /// GPU has written it.
    [[nodiscard]] bool Detach(Tegra::MaxwellDeviceMemoryManager& device_memory);

    /// Bytes of constant buffer contents copied by Detach (0 if not detached).
    [[nodiscard]] size_t SnapshotSizeBytes() const noexcept;

    /// Frees the constant buffers Detach copied, once translation is done: serialization only
    /// needs the recorded reads. The texture pool location is kept.
    void ReleaseSnapshotBuffers() noexcept;

    /// Texture pool entries of a detached environment (limit + 1), 0 if not detached.
    [[nodiscard]] u32 TexturePoolEntries() const noexcept;

    /// The texture reads translation made, read again from the pool (diagnostic for the late
    /// pool read on translation workers). GPU thread only.
    struct TexturePoolCheck {
        u32 reads{};
        u32 changed{};
        bool remapped{}; ///< The pool's GPU address no longer maps to the device range read
        bool dirty{};    ///< The GPU has written the pool since
    };
    [[nodiscard]] TexturePoolCheck CheckTextureReads(const Tegra::MemoryManager& current) const;

private:
    /// The 3D engine state a detached environment reads instead of the engine.
    struct EngineSnapshot {
        struct ConstBuffer {
            bool enabled{};
            u32 size{};
            /// One word longer than `size`, so a read at any offset below `size` stays inside.
            std::vector<u8> bytes;
        };
        std::array<ConstBuffer, Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers> const_buffers;

        /// The texture pool is located, not copied (it can hold thousands of entries); its
        /// entries are read through the device memory manager, which outlives the GPU address
        /// space.
        Tegra::MaxwellDeviceMemoryManager* device_memory{};
        GPUVAddr texture_pool_gpu{};
        DAddr texture_pool{};
        u32 texture_pool_limit{};
        bool via_header_index{};
        bool native_tiling{};

        u32 viewport_scale_offset_enabled{};
        ::Common::unordered_map<u64, Tegra::Engines::Maxwell3D::HLEReplacementAttributeType>
            replace_table;
    };

    /// Texture descriptor (TIC entry) of a texture handle, from the engine or the snapshot.
    Tegra::Texture::TICEntry ReadTextureDescriptor(u32 handle);

    /// Texture descriptor of a texture handle from the snapshot's pool.
    Tegra::Texture::TICEntry ReadSnapshotDescriptor(u32 handle) const;

    Tegra::Engines::Maxwell3D* maxwell3d{};
    size_t stage_index{};
    std::optional<EngineSnapshot> snapshot;
};

class ComputeEnvironment final : public GenericEnvironment {
public:
    explicit ComputeEnvironment() = default;
    explicit ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_,
                                std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~ComputeEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(
        [[maybe_unused]] u32 bank, [[maybe_unused]] u32 offset) override {
        return std::nullopt;
    }

private:
    Tegra::Engines::KeplerCompute* kepler_compute{};
};

/// Answers for reads a FileEnvironment has no record of (shader precache: programs from the game
/// files, never run, converted only to fill the driver's cache). Without it a missing read is an
/// error.
struct GuessMissing {
    u32 cbuf_bank0{0x10000000};  ///< Constant buffer 0 (driver constants, descriptor handles)
    u32 cbuf_other{0};           ///< Other constant buffers (game constants)
    Shader::TextureType texture_type{Shader::TextureType::ColorArray2D};
    Shader::TexturePixelFormat pixel_format{static_cast<Shader::TexturePixelFormat>(16)};
    bool storage_pointer_bank0{true};  ///< Bank 0 always held storage pointers in the test title's records
    bool storage_pointer_other{false}; ///< Banks 1-4 never did (95% identical SPIR-V offline)
};

class FileEnvironment final : public Shader::Environment {
public:
    FileEnvironment() = default;

    /// Missing reads get these answers instead of an error; the guess must outlive the
    /// environment.
    void SetGuessMissing(const GuessMissing* guess_) noexcept {
        guess = guess_;
    }
    ~FileEnvironment() override = default;

    FileEnvironment& operator=(FileEnvironment&&) noexcept = default;
    FileEnvironment(FileEnvironment&&) noexcept = default;

    FileEnvironment& operator=(const FileEnvironment&) = delete;
    FileEnvironment(const FileEnvironment&) = delete;

    void Deserialize(std::ifstream& file, bool with_mesh_state = false);

    [[nodiscard]] u64 ReadInstruction(u32 address) override;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const override;

    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const override;

    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;

    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;

    [[nodiscard]] u32 ReadViewportTransformState() override;

    [[nodiscard]] u32 LocalMemorySize() const override;

    [[nodiscard]] u32 SharedMemorySize() const override;

    [[nodiscard]] u32 TextureBoundBuffer() const override;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;

    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;

    [[nodiscard]] bool HasHLEMacroState() const override {
        return cbuf_replacements.size() != 0;
    }

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

private:
    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types;
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;
    std::array<u32, 3> workgroup_size{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    u32 texture_bound{};
    u32 read_lowest{};
    u32 read_highest{};
    u32 initial_offset{};
    u32 viewport_transform_state = 1;
    const GuessMissing* guess{};
};

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version,
                       bool with_mesh_state = false);

template <typename Key, typename Envs>
    requires std::is_trivially_copyable_v<Key>
        && std::has_unique_object_representations_v<Key>
void SerializePipeline(const Key& key, const Envs& envs, const std::filesystem::path& filename, u32 cache_version,
                       bool with_mesh_state = false) {
    SerializePipeline(std::span(reinterpret_cast<const char*>(&key), sizeof(key)), std::span(envs.data(), envs.size()), filename, cache_version,
                      with_mesh_state);
}

/// A cache file written by an older version whose records the current reader still parses: its
/// records, raw, to write the file again under the current version once they are translated.
struct CacheUpgrade {
    u32 from_version{};
    std::vector<std::string> records;
    std::vector<bool> is_compute;
};

/// Loads the records of a pipeline cache file. A file from min_compatible_version up to the
/// expected version is loaded too when `upgrade` is given (its records are kept there); older or
/// broken files are deleted.
void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics,
    bool with_mesh_state = false, u32 min_compatible_version = 0, CacheUpgrade* upgrade = nullptr);

/// Writes a cache file again from upgraded records under `cache_version`, through a temporary file
/// (the old one is kept as <name>.v<old>.bak until the new one is in place).
void RewritePipelines(const std::filesystem::path& filename, u32 cache_version, u32 old_version,
                      std::span<const std::string* const> records);

} // namespace VideoCommon
