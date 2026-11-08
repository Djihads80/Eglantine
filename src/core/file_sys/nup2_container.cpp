// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/nup2_container.h"

#include <cstring>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

namespace {

template <typename T>
T ReadLe(const std::vector<u8>& data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

std::string ReadString(const std::vector<u8>& data, std::size_t offset, std::size_t max) {
    std::size_t length = 0;
    while (length < max && data[offset + length] != 0) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), length);
}

} // namespace

bool Nup2Container::IsNup2File(const VirtualFile& file) {
    if (file == nullptr) {
        return false;
    }
    std::array<u8, 4> magic{};
    return file->Read(magic.data(), magic.size(), 0) == magic.size() && magic == Magic;
}

Nup2Container::Nup2Container(VirtualFile file_) : file(std::move(file_)) {
    if (!IsNup2File(file)) {
        return;
    }
    const std::vector<u8> super = file->ReadBytes(SuperblockSize, 0);
    if (super.size() != SuperblockSize) {
        LOG_ERROR(Loader, "NUP v2: truncated superblock");
        return;
    }
    if (ReadLe<u32>(super, 0x04) != FormatRevision) {
        LOG_ERROR(Loader, "NUP v2: unsupported format revision {}", ReadLe<u32>(super, 0x04));
        return;
    }
    title_id = ReadLe<u64>(super, 0x08);
    program_version = ReadLe<u32>(super, 0x10);
    const u64 file_size = ReadLe<u64>(super, 0x18);
    const u64 meta_offset = ReadLe<u64>(super, 0x20);
    const u64 meta_size = ReadLe<u64>(super, 0x28);
    romfs_offset = ReadLe<u64>(super, 0x30);
    romfs_size = ReadLe<u64>(super, 0x38);
    const u32 count = ReadLe<u32>(super, 0x40);
    display_version = ReadString(super, 0x48, 32);
    title_name = ReadString(super, 0x68, 128);
    if ((ReadLe<u32>(super, 0x14) & FlagRomFsSha256) != 0) {
        Sha256Digest digest{};
        std::memcpy(digest.data(), super.data() + RomFsSha256Offset, digest.size());
        romfs_sha256 = digest;
    }

    const u64 actual_size = file->GetSize();
    if (file_size != actual_size || romfs_offset % RomFsAlignment != 0 || romfs_offset > actual_size ||
        romfs_size > actual_size - romfs_offset || meta_offset > actual_size ||
        meta_size > actual_size - meta_offset || meta_size < u64{count} * EntrySize) {
        LOG_ERROR(Loader, "NUP v2: inconsistent header (file {} bytes, header says {})", actual_size, file_size);
        return;
    }
    const std::vector<u8> meta = file->ReadBytes(static_cast<std::size_t>(u64{count} * EntrySize),
                                                 static_cast<std::size_t>(meta_offset));
    if (meta.size() != u64{count} * EntrySize) {
        LOG_ERROR(Loader, "NUP v2: truncated ExeFS table");
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        const std::size_t base = std::size_t{i} * EntrySize;
        ExeFsEntry entry;
        entry.offset = ReadLe<u64>(meta, base);
        entry.size = ReadLe<u64>(meta, base + 8);
        entry.name = ReadString(meta, base + 16, 48);
        if (entry.offset > actual_size || entry.size > actual_size - entry.offset || entry.name.empty()) {
            LOG_ERROR(Loader, "NUP v2: bad ExeFS entry {}", i);
            return;
        }
        exefs_entries.push_back(std::move(entry));
    }
    valid = true;
}

VirtualDir Nup2Container::GetExeFsDir() const {
    std::vector<VirtualFile> files;
    for (const auto& entry : exefs_entries) {
        if (entry.name == IconName) {
            continue;
        }
        files.push_back(std::make_shared<OffsetVfsFile>(file, static_cast<std::size_t>(entry.size),
                                                        static_cast<std::size_t>(entry.offset), entry.name));
    }
    return std::make_shared<VectorVfsDirectory>(std::move(files), std::vector<VirtualDir>{}, "exefs");
}

std::vector<u8> Nup2Container::GetIcon() const {
    for (const auto& entry : exefs_entries) {
        if (entry.name == IconName) {
            return file->ReadBytes(static_cast<std::size_t>(entry.size), static_cast<std::size_t>(entry.offset));
        }
    }
    return {};
}

VirtualFile Nup2Container::GetRomFs() const {
    return std::make_shared<OffsetVfsFile>(file, static_cast<std::size_t>(romfs_size),
                                           static_cast<std::size_t>(romfs_offset), "romfs");
}

} // namespace FileSys
