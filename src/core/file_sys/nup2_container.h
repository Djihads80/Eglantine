// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

/**
 * NUP v2: one program, stored so that the game's RomFS is read straight out of the file.
 *
 * Everything is little-endian. The RomFS is stored raw and byte-identical to the image the game sees, so a read
 * at offset X of the game's storage is a read at RomFsOffset + X of this file: no table, no translation, no
 * per-file open. (A later revision may add an extent map to reorder or compress data; the identity layout is the
 * baseline.)
 *
 *   0x0000  Superblock (4096 bytes)
 *   0x1000  Meta: ExeFS entry table (exefs_count * 64 bytes)
 *   ...     ExeFS member files, raw, each starting on a 4096-byte boundary
 *   ...     RomFS image, raw, starting on a 65536-byte boundary, running to the end of the file
 *
 * The header (superblock and ExeFS table) fixes the offset and size of every member and of the RomFS before any
 * data exists, so a writer can size the file once and place each member, or any slice of the RomFS (which sits at
 * RomFsOffset + its offset in the game's image), from any thread in any order. The gaps between members are zeros.
 *
 * Superblock:
 *   0x00 u8[4]  magic "NUP\x02"
 *   0x04 u32    format revision (1)
 *   0x08 u64    title id
 *   0x10 u32    program version (major << 16 | minor << 8 | patch)
 *   0x14 u32    flags (bit 0: the RomFS SHA-256 at 0xE8 is present)
 *   0x18 u64    file size
 *   0x20 u64    meta offset
 *   0x28 u64    meta size
 *   0x30 u64    RomFS offset
 *   0x38 u64    RomFS size
 *   0x40 u32    ExeFS entry count
 *   0x44 u32    reserved (0)
 *   0x48 char[32]   display version (NUL padded)
 *   0x68 char[128]  title name (NUL padded)
 *   0xE8 u8[32]     SHA-256 of the RomFS image (valid when flag bit 0 is set; zeros otherwise)
 *
 * ExeFS entry (64 bytes): u64 offset, u64 size, char name[48] (NUL padded).
 */
class Nup2Container {
public:
    static constexpr std::array<u8, 4> Magic{'N', 'U', 'P', 0x02};
    static constexpr u32 FormatRevision = 1;
    static constexpr u64 SuperblockSize = 0x1000;
    static constexpr u64 ExeFsAlignment = 0x1000;
    static constexpr u64 RomFsAlignment = 0x10000;
    static constexpr std::size_t EntrySize = 64;
    static constexpr u32 FlagRomFsSha256 = 1;
    static constexpr std::size_t RomFsSha256Offset = 0xE8;
    using Sha256Digest = std::array<u8, 32>;
    /// Optional table entry holding the game list icon (not part of the ExeFS the game sees).
    static constexpr std::string_view IconName = "icon.dat";

    struct ExeFsEntry {
        u64 offset{};
        u64 size{};
        std::string name;
    };

    /// Quick magic check without a full parse.
    static bool IsNup2File(const VirtualFile& file);

    explicit Nup2Container(VirtualFile file_);

    bool IsValid() const {
        return valid;
    }
    u64 GetTitleId() const {
        return title_id;
    }
    u32 GetProgramVersion() const {
        return program_version;
    }
    const std::string& GetDisplayVersion() const {
        return display_version;
    }
    const std::string& GetTitleName() const {
        return title_name;
    }
    const std::vector<ExeFsEntry>& GetExeFsEntries() const {
        return exefs_entries;
    }
    /// The SHA-256 the writer recorded for the RomFS image; empty for files written without one.
    const std::optional<Sha256Digest>& GetRomFsSha256() const {
        return romfs_sha256;
    }

    /// The ExeFS members as a directory of views into the container.
    VirtualDir GetExeFsDir() const;
    /// The embedded icon, empty when the file has none.
    std::vector<u8> GetIcon() const;
    /// The RomFS image exactly as the game reads it: a view into the container, no copy.
    VirtualFile GetRomFs() const;

private:
    VirtualFile file;
    bool valid{};
    u64 title_id{};
    u32 program_version{};
    u64 romfs_offset{};
    u64 romfs_size{};
    std::string display_version;
    std::string title_name;
    std::vector<ExeFsEntry> exefs_entries;
    std::optional<Sha256Digest> romfs_sha256;
};

} // namespace FileSys
