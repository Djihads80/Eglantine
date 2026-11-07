// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/loader/loader.h"

namespace FileSys {

class VectorVfsDirectory;

/**
 * NUP v2 container reader (SPEC.md v2): a bare HFS0 partition whose first
 * entry, meta.nup, carries versioning, grouping and integrity for the whole
 * file. Entries are stored raw and 0x200-aligned; all identity hashes are
 * over logical bytes.
 *
 * Layout:
 *   header 0x10: magic "HFS0", entry count, string table size (padded so the
 *                data region starts on a 0x200 boundary), reserved
 *   entry table: N * 0x40 (data offset, size, name offset, hashed size,
 *                reserved, prefix hash; meta.nup stores hashed size 0)
 *   string table: NUL-terminated names in entry order
 *   data region: entry data; meta.nup is entry 0
 *
 * meta.nup (magic "NUP\x02"): 0x40-byte header with the SHA-256 of container
 * bytes [0, data_region_start) and a feature-flags field (must be zero), then
 * one 0x68-byte record per package (kind, flags, tlv size, title id,
 * application id, version, file count, 0x20-byte content hash, 0x20-byte base
 * content hash, first entry index, reserved), TLVs, the package file table,
 * and a trailing SHA-256 of everything before it.
 */
class Nup2Container {
public:
    static constexpr std::array<u8, 4> HeaderMagic{'H', 'F', 'S', '0'};
    static constexpr std::array<u8, 4> MetaMagic{'N', 'U', 'P', 0x02};
    static constexpr std::string_view MetaEntryName = "meta.nup";

    enum class PackageKind : u8 {
        Base = 0,
        Update = 1,
        Addon = 2,
    };

    struct FileRow {
        std::string path;
        u64 size{};
        std::array<u8, 0x20> sha256{};
    };

    struct PackageInfo {
        PackageKind kind{};
        u8 flags{};
        u64 title_id{};
        u64 application_id{};
        u32 version{};
        u64 first_entry_index{};
        std::array<u8, 0x20> content_hash{};
        std::array<u8, 0x20> base_content_hash{};
        std::vector<FileRow> files;
        std::string title_name;
        std::string publisher;
        std::string display_version;
        std::string build_id;
        std::string source_dump;
        std::vector<u8> icon;
        std::vector<std::pair<std::string, std::string>> localized_titles;
        std::vector<std::pair<std::string, std::vector<u8>>> localized_icons;

        bool IsDiff() const {
            return (flags & 1) != 0;
        }
    };

    /// True when the file starts with HFS0 and its first entry is meta.nup
    /// whose data begins with the "NUP\x02" magic. Cheap check, no parse.
    static bool IsNup2File(const VirtualFile& file);

    explicit Nup2Container(VirtualFile file_);

    Loader::ResultStatus GetStatus() const {
        return status;
    }

    const std::vector<PackageInfo>& GetPackages() const {
        return packages;
    }

    std::optional<std::size_t> FindBasePackage() const;
    /// Highest version among update packages, if any.
    std::optional<std::size_t> FindBestUpdatePackage() const;

    /// The scope directory prefix every entry of this package lives under:
    /// "base", "update/<decimal version>" or "addon/<console index>".
    std::string ScopePrefix(const PackageInfo& package) const;

    /// Zero-copy view into the container at the entry's data.
    VirtualFile OpenEntry(std::size_t entry_index);

    /// Builds the package's scope tree (subdirectories mirror the paths).
    VirtualDir GetPackageDir(std::size_t package_index);
    VirtualDir GetExeFSDir(std::size_t package_index);
    VirtualDir GetRomFSDir(std::size_t package_index);

    /// bind + package + full: re-hash every entry (prefix and file table).
    Loader::ResultStatus VerifyIntegrity(
        std::function<bool(std::size_t, std::size_t)> progress_callback) const;

    std::size_t GetEntryCount() const {
        return entries.size();
    }

private:
    struct Entry {
        std::string name;
        u64 data_offset{};
        u64 size{};
        u32 name_offset{};
        u32 hashed_size{};
        std::array<u8, 0x20> prefix_hash{};
    };

    bool Parse();
    bool ParseMeta();
    bool ParseTlvs(PackageInfo& package, const std::vector<u8>& tlv_block);
    bool ParseFileRows(PackageInfo& package, const u8* data, std::size_t size,
                       std::size_t& pos) const;
    bool ReadMetaEntryBytes(std::vector<u8>& out) const;
    std::array<u8, 0x20> Sha256(const u8* data, std::size_t size) const;
    std::array<u8, 0x20> Sha256Span(const std::vector<u8>& data) const;
    bool VerifyPinning() const;

    VirtualFile file;
    Loader::ResultStatus status{Loader::ResultStatus::ErrorNullFile};

    u64 file_size{};
    std::size_t entry_count{};
    std::size_t string_table_size{};
    std::size_t data_start{};
    std::vector<Entry> entries;
    std::vector<PackageInfo> packages;
    std::vector<VirtualDir> scope_dir_cache;
};

} // namespace FileSys
