// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace FileSys {

/// A zstd member too large for the in-memory cache, decompressed once into a temporary file.
struct NupSpillFile {
    ~NupSpillFile();

    std::mutex mutex;
    std::filesystem::path path;
    Common::FS::IOFile file;
    bool attempted{};
};

/// LRU over decompressed zstd extents, shared by the files of one container.
struct NupDecompressCache {
    static constexpr std::size_t DefaultCap = 512 * 1024 * 1024;

    using Entry = std::pair<std::size_t, std::vector<u8>>;

    std::mutex mutex;
    std::list<Entry> lru;
    std::unordered_map<std::size_t, std::list<Entry>::iterator> index;
    std::size_t bytes{};
    std::size_t cap{DefaultCap};
    std::filesystem::path spill_dir;
    std::unordered_map<std::size_t, std::shared_ptr<NupSpillFile>> spills;

    std::vector<u8>* Find(std::size_t file_index) {
        const auto it = index.find(file_index);
        if (it == index.end()) {
            return nullptr;
        }
        lru.splice(lru.begin(), lru, it->second);
        return &it->second->second;
    }

    /// Takes ownership of `data` only when it is inserted.
    void Insert(std::size_t file_index, std::vector<u8>& data) {
        if (index.contains(file_index)) {
            return;
        }
        while (bytes + data.size() > cap && !lru.empty()) {
            bytes -= lru.back().second.size();
            index.erase(lru.back().first);
            lru.pop_back();
        }
        if (bytes + data.size() > cap) {
            return;
        }
        bytes += data.size();
        lru.emplace_front(file_index, std::move(data));
        index.emplace(file_index, lru.begin());
    }

    std::shared_ptr<NupSpillFile> GetSpill(std::size_t file_index) {
        auto& spill = spills[file_index];
        if (!spill) {
            spill = std::make_shared<NupSpillFile>();
        }
        return spill;
    }
};

/**
 * NUP (NX Package) container: stores loose ExeFS/RomFS trees for a base program,
 * its updates and add-on content in one file. Layout:
 *
 *   Superblock (0x80 bytes, magic "NUP\x01")
 *   Table of Contents (packages, dir/file nodes, children array, string pool)
 *   Blob region (per-file extents, raw or zstd codec, 16-byte aligned)
 *
 * Per-package meta TLVs carry NPDM bytes, title names (default + localized),
 * icons and a display version. All integers little-endian.
 */
class NupContainer {
public:
    static constexpr std::array<u8, 4> HeaderMagic{'N', 'U', 'P', 0x01};

    enum class PackageKind : u8 {
        Base = 0,
        Update = 1,
        Addon = 2,
    };


    struct PackageInfo {
        u64 title_id{};
        u32 version{};
        PackageKind kind{};
        bool standalone_update{};
        u32 root_dir{};
        u32 exefs_dir{};
        u32 romfs_dir{};
        u64 meta_offset{};
        u32 meta_size{};

        std::vector<u8> npdm;
        std::vector<u8> nacp;
        std::string title_name;
        std::string publisher;
        std::string source_dump;
        std::vector<std::pair<std::string, std::string>> localized_titles;
        std::vector<u8> icon;
        std::vector<std::pair<std::string, std::vector<u8>>> localized_icons;
        std::string display_version;
    };

    /// Quick magic check without a full parse.
    static bool IsNupFile(const VirtualFile& file);

    /// Recursively merges overlay into a copy of base; overlay paths win.
    static VirtualDir MergeShadowDirs(const VirtualDir& base, const VirtualDir& overlay);

    /// Zstd members larger than half of `memory_cache_cap` are decompressed to a temporary file
    /// in `spill_dir` (the cache directory by default) instead of being held in memory.
    explicit NupContainer(VirtualFile file_,
                          std::size_t memory_cache_cap = NupDecompressCache::DefaultCap,
                          std::filesystem::path spill_dir_ = {});
    ~NupContainer();

    Loader::ResultStatus GetStatus() const {
        return status;
    }

    const std::vector<PackageInfo>& GetPackages() const {
        return packages;
    }

    std::optional<std::size_t> FindPackage(PackageKind kind,
                                           std::optional<u64> title_id = std::nullopt) const;
    std::optional<std::size_t> FindBasePackage() const;
    /// Highest version among Update packages, if any.
    std::optional<std::size_t> FindBestUpdatePackage() const;

    /// Opens a member file by node index; raw extents are zero-copy views into
    /// the container, zstd extents are lazily decompressed, cached in memory
    /// or, when large, in a temporary file.
    VirtualFile OpenFile(std::size_t file_index);

    VirtualDir GetExeFSDir(std::size_t package_index);
    VirtualDir GetRomFSDir(std::size_t package_index);
    VirtualDir GetPackageDir(std::size_t package_index);

    /// Newest usable add-on RomFS per title belonging to this application.
    std::vector<std::pair<u64, VirtualFile>> GetAddonRomFS(u64 program_id);

    /// Stream-hashes every member file against its stored SHA-256.
    Loader::ResultStatus VerifyIntegrity(
        std::function<bool(std::size_t, std::size_t)> progress_callback) const;

    std::size_t GetFileCount() const {
        return files.size();
    }

private:
    struct DirNode {
        std::string name;
        u32 first_child{};
        u32 child_count{};
    };

    struct FileNode {
        std::string name;
        u64 size{};
        std::array<u8, 0x20> sha256{};
        u64 extent_offset{};
        u64 extent_size{};
        u8 codec{};
    };

    bool Parse();
    bool ParseMeta(PackageInfo& package);
    bool ValidateDirTree() const;
    void BuildDirTree(u32 dir_index, VectorVfsDirectory& out);
    VirtualDir BuildPackageDir(u32 dir_index);
    std::vector<u8> ReadExtentRaw(const FileNode& node) const;

    VirtualFile file;
    Loader::ResultStatus status{Loader::ResultStatus::ErrorNullFile};

    u64 toc_offset{};
    u64 toc_size{};
    u64 blob_offset{};
    u64 file_size{};

    std::vector<DirNode> dirs;
    std::vector<FileNode> files;
    std::vector<u32> children;
    std::vector<PackageInfo> packages;

    std::vector<VirtualDir> exefs_dir_cache;
    std::vector<VirtualDir> romfs_dir_cache;

    std::shared_ptr<NupDecompressCache> cache;
};

} // namespace FileSys
