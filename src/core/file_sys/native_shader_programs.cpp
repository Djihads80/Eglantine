// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "common/cityhash.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/file_sys/native_shader_programs.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/loader/loader.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_scan.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace FileSys {
namespace {

// Changed with the scan's results (program_scan.h) or this file's layout.
constexpr std::array<char, 8> CacheMagic{'N', 'X', 'P', 'R', 'G', '0', '0', '6'};
// Shader containers are far smaller (Mario Kart World: 114 MB at most, decompressed); larger
// files are packs of other data, not worth holding in memory whole.
constexpr size_t MaxDecompressedSize = size_t{512} << 20;
constexpr u64 MaxFileSize = u64{256} << 20;
// Limits a program list read back must stay within, far above any title's.
constexpr u32 MaxCachedPrograms = 1U << 20;
constexpr u32 MaxCachedHeaderSize = 0x100;
constexpr u32 MaxCachedCodeSize = 1U << 24;
constexpr u32 MaxCachedEntries = 1U << 24;

// Media and physics data never holds shader code: skipping it saves most of the scan.
constexpr std::array SkippedExtensions{
    std::string_view{".bntx"},  std::string_view{".bwav"}, std::string_view{".bfstm"},
    std::string_view{".webm"},  std::string_view{".mp4"},  std::string_view{".bphsh"},
    std::string_view{".bphsc"}, std::string_view{".extm"}, std::string_view{".mate"},
    std::string_view{".norm"},  std::string_view{".hght"},
};

// An extension counts whole, also before a compression suffix (.bntx.zs), not as part of a longer
// one (.material).
bool IsSkipped(std::string_view name) {
    return std::ranges::any_of(SkippedExtensions, [name](std::string_view extension) {
        for (size_t at = name.find(extension); at != std::string_view::npos;
             at = name.find(extension, at + 1)) {
            const size_t end = at + extension.size();
            if (end == name.size() || name[end] == '.') {
                return true;
            }
        }
        return false;
    });
}

std::optional<Shader::Ampere::NativeProgramSet> ReadCache(const std::filesystem::path& path,
                                                          u64 fingerprint) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 8> magic{};
    u64 stored{};
    u32 count{};
    if (!file.read(magic.data(), magic.size()) || magic != CacheMagic ||
        !file.read(reinterpret_cast<char*>(&stored), sizeof(stored)) || stored != fingerprint ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count)) || count > MaxCachedPrograms) {
        return {};
    }
    Shader::Ampere::NativeProgramSet set;
    set.programs.resize(count);
    for (auto& program : set.programs) {
        u32 header_size{};
        u32 code_size{};
        if (!file.read(reinterpret_cast<char*>(&header_size), sizeof(header_size)) ||
            !file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size)) ||
            header_size > MaxCachedHeaderSize || code_size > MaxCachedCodeSize) {
            return {};
        }
        program.header.resize(header_size);
        program.code.resize(code_size);
        if (!file.read(reinterpret_cast<char*>(program.header.data()), header_size) ||
            !file.read(reinterpret_cast<char*>(program.code.data()), code_size)) {
            return {};
        }
    }
    u32 pair_count{};
    if (!file.read(reinterpret_cast<char*>(&pair_count), sizeof(pair_count)) ||
        pair_count > MaxCachedEntries) {
        return {};
    }
    set.pairs.resize(pair_count);
    if (!file.read(reinterpret_cast<char*>(set.pairs.data()),
                   static_cast<std::streamsize>(pair_count * sizeof(set.pairs[0])))) {
        return {};
    }
    u32 compute_count{};
    if (!file.read(reinterpret_cast<char*>(&compute_count), sizeof(compute_count)) ||
        compute_count > MaxCachedEntries) {
        return {};
    }
    set.compute.resize(compute_count);
    if (!file.read(reinterpret_cast<char*>(set.compute.data()),
                   static_cast<std::streamsize>(compute_count * sizeof(set.compute[0])))) {
        return {};
    }
    const auto valid = [count](u32 index) { return index < count; };
    if (!std::ranges::all_of(
            set.pairs, [&](const auto& pair) { return valid(pair.first) && valid(pair.second); }) ||
        !std::ranges::all_of(set.compute,
                             [&](const auto& dispatch) { return valid(dispatch.program); })) {
        return {};
    }
    return set;
}

void WriteCache(const std::filesystem::path& path, u64 fingerprint,
                const Shader::Ampere::NativeProgramSet& set) {
    const auto& programs = set.programs;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary{path};
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const u32 count = static_cast<u32>(programs.size());
        file.write(CacheMagic.data(), CacheMagic.size());
        file.write(reinterpret_cast<const char*>(&fingerprint), sizeof(fingerprint));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& program : programs) {
            const u32 header_size = static_cast<u32>(program.header.size());
            const u32 code_size = static_cast<u32>(program.code.size());
            file.write(reinterpret_cast<const char*>(&header_size), sizeof(header_size));
            file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size));
            file.write(reinterpret_cast<const char*>(program.header.data()), header_size);
            file.write(reinterpret_cast<const char*>(program.code.data()), code_size);
        }
        const u32 pair_count = static_cast<u32>(set.pairs.size());
        file.write(reinterpret_cast<const char*>(&pair_count), sizeof(pair_count));
        file.write(reinterpret_cast<const char*>(set.pairs.data()),
                   static_cast<std::streamsize>(pair_count * sizeof(set.pairs[0])));
        const u32 compute_count = static_cast<u32>(set.compute.size());
        file.write(reinterpret_cast<const char*>(&compute_count), sizeof(compute_count));
        file.write(reinterpret_cast<const char*>(set.compute.data()),
                   static_cast<std::streamsize>(compute_count * sizeof(set.compute[0])));
        file.close();
        if (!file) {
            LOG_ERROR(Loader, "Failed to write the native shader program list {}",
                      Common::FS::PathToUTF8String(path));
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
}

// Identifies the files the game sees (base, update and mods) without reading their contents.
void HashTree(const VirtualDir& dir, u64& hash) {
    for (const auto& file : dir->GetFiles()) {
        const std::string name = file->GetName();
        const u64 size = file->GetSize();
        hash = Common::CityHash64WithSeed(name.data(), name.size(), hash);
        hash = Common::CityHash64WithSeed(reinterpret_cast<const char*>(&size), sizeof(size), hash);
    }
    for (const auto& subdirectory : dir->GetSubdirectories()) {
        const std::string name = subdirectory->GetName() + '/';
        hash = Common::CityHash64WithSeed(name.data(), name.size(), hash);
        HashTree(subdirectory, hash);
    }
}

// The RomFS the game reads: the base image with its update and mods applied.
VirtualFile OpenApplicationRomFS(Core::System& system) {
    if (const auto* const process = system.ApplicationProcess()) {
        u64 program_id{};
        std::shared_ptr<Service::FileSystem::SaveDataController> save_data_controller;
        std::shared_ptr<Service::FileSystem::RomFsController> romfs_controller;
        if (system.GetFileSystemController()
                .OpenProcess(&program_id, &save_data_controller, &romfs_controller,
                             process->GetProcessId())
                .IsSuccess() &&
            romfs_controller) {
            if (auto romfs = romfs_controller->OpenRomFSCurrentProcess()) {
                return romfs;
            }
        }
    }
    VirtualFile romfs;
    if (system.GetAppLoader().ReadRomFS(romfs) != Loader::ResultStatus::Success) {
        return nullptr;
    }
    return romfs;
}

struct ScanState {
    std::unordered_map<u64, u32> indices; ///< Header and code hash -> index in set.programs
    std::set<std::pair<u32, u32>> pairs;
    std::set<std::tuple<u32, std::array<u32, 3>, u32>> compute;
    Shader::Ampere::NativeProgramSet set;
    size_t files{};
};

void ScanDirectory(const VirtualDir& dir, std::stop_token stop, ScanState& state) {
    for (const auto& file : dir->GetFiles()) {
        if (stop.stop_requested()) {
            return;
        }
        const std::string name = file->GetName();
        if (IsSkipped(name) || file->GetSize() > MaxFileSize) {
            continue;
        }
        std::vector<u8> data = file->ReadAllBytes();
        if (name.ends_with(".zs")) {
            auto decompressed =
                Common::Compression::TryDecompressDataZSTD(data, MaxDecompressedSize);
            if (!decompressed) {
                continue;
            }
            data = std::move(*decompressed);
        }
        ++state.files;
        const auto scanned = Shader::Ampere::ScanNativePrograms(data);
        std::vector<u32> indices(scanned.size());
        for (size_t i = 0; i < scanned.size(); ++i) {
            auto image = Shader::Ampere::CopyNativeProgram(data, scanned[i]);
            std::vector<Shader::Ampere::Instruction> code(image.code.size() /
                                                          sizeof(Shader::Ampere::Instruction));
            std::memcpy(code.data(), image.code.data(), code.size() * sizeof(code[0]));
            // The same code with another header (outputs, attributes) is another program.
            const u64 identity = Common::CityHash64WithSeed(
                reinterpret_cast<const char*>(image.header.data()), image.header.size(),
                Shader::Ampere::NativeProgramKey(code));
            const auto [it, inserted] =
                state.indices.emplace(identity, static_cast<u32>(state.set.programs.size()));
            if (inserted) {
                state.set.programs.push_back(std::move(image));
            }
            indices[i] = it->second;
        }
        for (const auto& [vertex, pixel] : Shader::Ampere::ScanProgramPairs(data, scanned)) {
            state.pairs.emplace(indices[vertex], indices[pixel]);
        }
        for (const auto& dispatch : Shader::Ampere::ScanComputeDispatches(data, scanned)) {
            state.compute.emplace(indices[dispatch.program], dispatch.workgroup,
                                  dispatch.shared_memory);
        }
    }
    for (const auto& subdirectory : dir->GetSubdirectories()) {
        ScanDirectory(subdirectory, stop, state);
    }
}

} // Anonymous namespace

Shader::Ampere::NativeProgramSet CollectNativeShaderPrograms(
    const VirtualFile& romfs, const std::filesystem::path& cache_file, std::stop_token stop) {
    if (!romfs) {
        return {};
    }
    const auto start = std::chrono::steady_clock::now();
    const VirtualDir root = ExtractRomFS(romfs);
    if (!root) {
        return {};
    }
    u64 fingerprint = romfs->GetSize();
    HashTree(root, fingerprint);
    if (auto cached = ReadCache(cache_file, fingerprint)) {
        LOG_INFO(Loader,
                 "Native shader programs: {}, {} vertex and pixel pairs and {} compute dispatches "
                 "read from {}",
                 cached->programs.size(), cached->pairs.size(), cached->compute.size(),
                 Common::FS::PathToUTF8String(cache_file));
        return std::move(*cached);
    }
    ScanState state;
    ScanDirectory(root, stop, state);
    if (stop.stop_requested()) {
        return {};
    }
    state.set.pairs.assign(state.pairs.begin(), state.pairs.end());
    for (const auto& [program, workgroup, shared] : state.compute) {
        state.set.compute.push_back({program, workgroup, shared});
    }
    LOG_INFO(
        Loader,
        "Native shader programs: {}, {} vertex and pixel pairs and {} compute dispatches found "
        "in {} files in {:.1f} s",
        state.set.programs.size(), state.set.pairs.size(), state.set.compute.size(), state.files,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    // Kept even when empty, so a title without native programs is not scanned on every boot.
    WriteCache(cache_file, fingerprint, state.set);
    return std::move(state.set);
}

void PrecompileApplicationShaders(Core::System& system, u64 program_id) {
    // Pipelines are precompiled when the disk shader cache loads, which is skipped when it is off.
    if (!Settings::CurrentProgramIsSwitch2() || !Settings::values.precompile_shaders.GetValue() ||
        !Settings::values.use_disk_shader_cache.GetValue()) {
        return;
    }
    using Collector = VideoCore::RasterizerInterface::NativeProgramCollector;
    system.Renderer().ReadRasterizer()->PrecompileNativePrograms([&system,
                                                                  program_id]() -> Collector {
        // Opened here rather than on the scan thread: shutdown resets the filesystem controller
        // and the loader before the renderer, which stops the scan.
        const VirtualFile romfs = OpenApplicationRomFS(system);
        if (!romfs) {
            LOG_WARNING(Loader, "Native shader precompile: the application has no RomFS");
            return {};
        }
        const auto cache_file = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                                fmt::format("{:016x}", program_id) / "native_programs.bin";
        return [romfs, cache_file](std::stop_token stop) -> Shader::Ampere::NativeProgramSet {
            try {
                return CollectNativeShaderPrograms(romfs, cache_file, stop);
            } catch (const std::exception& exception) {
                LOG_ERROR(Loader, "Native shader precompile: the RomFS scan failed: {}",
                          exception.what());
                return {};
            }
        };
    });
}

} // namespace FileSys
