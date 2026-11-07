// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/nup2_container.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {
namespace {

constexpr std::size_t HeaderSize = 0x10;
constexpr std::size_t EntrySize = 0x40;
constexpr u64 Alignment = 0x200;
constexpr std::size_t MetaHeaderSize = 0x40;
constexpr std::size_t PackageFixedSize = 0x68;
constexpr std::size_t DigestSize = 0x20;
constexpr std::size_t MaxMetaRecordSize = 1ULL << 31;

constexpr u8 FlagDiff = 0x01;

enum TlvId : u16 {
    TlvIcon = 0x0003,
    TlvDisplayVersion = 0x0004,
    TlvTitleName = 0x0005,
    TlvPublisher = 0x0006,
    TlvLocalizedTitle = 0x0009,
    TlvLocalizedIcon = 0x000A,
    TlvBuildId = 0x000B,
    TlvSourceDump = 0x000C,
};

u16 ReadU16(const u8* data) {
    return static_cast<u16>(data[0]) | static_cast<u16>(static_cast<u16>(data[1]) << 8);
}

u32 ReadU32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

u64 ReadU64(const u8* data) {
    u64 value{};
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<u64>(data[i]) << (8 * i);
    }
    return value;
}

// SPEC Section 4: names are '/'-separated and case-sensitive; absolute paths,
// traversal parts, backslashes and empty components all fail the container at
// open time, before anything is written to disk (issue #24).
bool IsValidPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find("//") != std::string_view::npos || path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::string_view rest = path;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto part = rest.substr(0, slash == std::string_view::npos ? rest.size() : slash);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    return true;
}

} // namespace

bool Nup2Container::IsNup2File(const VirtualFile& file) {
    if (file == nullptr || file->GetSize() < HeaderSize + EntrySize) {
        return false;
    }
    const auto header = file->ReadBytes(HeaderSize, 0);
    if (ReadU32(header.data()) != 0x30534648) { // "HFS0" as a little-endian u32
        return false;
    }
    const u32 count = ReadU32(header.data() + 4);
    const u32 table_size = ReadU32(header.data() + 8);
    const u64 entries_end = HeaderSize + EntrySize * count;
    if (count == 0 || entries_end + table_size > file->GetSize() ||
        table_size < MetaEntryName.size() + 1) {
        return false;
    }
    const auto entry0 = file->ReadBytes(EntrySize, HeaderSize);
    const u64 data_offset = ReadU64(entry0.data() + 0x00);
    const u32 name_offset = ReadU32(entry0.data() + 0x10);
    if (name_offset + MetaEntryName.size() + 1 > table_size) {
        return false;
    }
    const auto names = file->ReadBytes(table_size - name_offset, entries_end + name_offset);
    if (names.size() < MetaEntryName.size() + 1) {
        return false;
    }
    if (std::string_view(reinterpret_cast<const char*>(names.data()), MetaEntryName.size()) !=
        MetaEntryName) {
        return false;
    }
    // entry data offsets are relative to the data region, which starts after
    // the 0x200-aligned string table
    const u64 data_start = (entries_end + table_size + Alignment - 1) & ~(Alignment - 1);
    const auto meta_magic = file->ReadBytes(4, data_start + data_offset);
    return meta_magic.size() == 4 && meta_magic[0] == MetaMagic[0] && meta_magic[1] == MetaMagic[1] &&
           meta_magic[2] == MetaMagic[2] && meta_magic[3] == MetaMagic[3];
}

Nup2Container::Nup2Container(VirtualFile file_) : file(std::move(file_)) {
    if (file == nullptr) {
        return;
    }
    if (Parse() && ParseMeta()) {
        status = Loader::ResultStatus::Success;
        scope_dir_cache.resize(packages.size());
    } else if (status == Loader::ResultStatus::ErrorNullFile) {
        status = Loader::ResultStatus::ErrorNUPBadFormat;
    }
}

std::array<u8, 0x20> Nup2Container::Sha256(const u8* data, std::size_t size) const {
    std::array<u8, 0x20> out{};
    unsigned int length = 0;
    EVP_Digest(data, size, out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

std::array<u8, 0x20> Nup2Container::Sha256Span(const std::vector<u8>& data) const {
    return Sha256(data.data(), data.size());
}

bool Nup2Container::Parse() {
    file_size = file->GetSize();
    if (file_size < HeaderSize + EntrySize) {
        LOG_ERROR(Loader, "NUP2 container is smaller than an HFS0 header");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const auto header = file->ReadBytes(HeaderSize, 0);
    if (ReadU32(header.data()) != 0x30534648) { // "HFS0" as a little-endian u32
        LOG_ERROR(Loader, "NUP2 container magic is not HFS0");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const u32 declared_count = ReadU32(header.data() + 4);
    const u32 declared_table = ReadU32(header.data() + 8);
    if (ReadU32(header.data() + 12) != 0) {
        LOG_ERROR(Loader, "NUP2 header reserved field is not zero");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const u64 entries_end = HeaderSize + EntrySize * declared_count;
    const u64 declared_prefix_end = entries_end + declared_table;
    if (declared_prefix_end > file_size) {
        LOG_ERROR(Loader,
                  "NUP2 declares {} entries and a {} byte string table, which overruns the "
                  "{} byte container",
                  declared_count, declared_table, file_size);
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    if (declared_count == 0) {
        LOG_ERROR(Loader, "NUP2 container has no entries");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const u64 aligned_data_start =
        (declared_prefix_end + Alignment - 1) & ~(Alignment - 1);
    const auto prefix = file->ReadBytes(static_cast<std::size_t>(declared_prefix_end), 0);
    if (prefix.size() != declared_prefix_end) {
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }

    entry_count = declared_count;
    string_table_size = declared_table;
    data_start = static_cast<std::size_t>(aligned_data_start);
    const u8* table = prefix.data() + entries_end;

    entries.resize(entry_count);
    for (std::size_t index = 0; index < entry_count; ++index) {
        const u8* record = prefix.data() + HeaderSize + EntrySize * index;
        Entry& entry = entries[index];
        entry.data_offset = ReadU64(record + 0x00);
        entry.size = ReadU64(record + 0x08);
        entry.name_offset = ReadU32(record + 0x10);
        entry.hashed_size = ReadU32(record + 0x14);
        if (ReadU64(record + 0x18) != 0) {
            LOG_ERROR(Loader, "NUP2 entry {} reserved field is not zero", index);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        std::memcpy(entry.prefix_hash.data(), record + 0x20, 0x20);
        if (entry.name_offset >= string_table_size) {
            LOG_ERROR(Loader, "NUP2 entry {} name offset is outside the string table", index);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        const u8* name_start = table + entry.name_offset;
        const void* nul =
            std::memchr(name_start, '\0', string_table_size - entry.name_offset);
        if (nul == nullptr) {
            LOG_ERROR(Loader, "NUP2 entry {} name is not NUL-terminated", index);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        entry.name = std::string(reinterpret_cast<const char*>(name_start),
                                 static_cast<std::size_t>(static_cast<const u8*>(nul) - name_start));
        if (entry.data_offset % Alignment != 0) {
            LOG_ERROR(Loader, "NUP2 entry {} data is not 0x200-aligned", index);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
    }
    if (entries[0].name != MetaEntryName) {
        LOG_ERROR(Loader, "NUP2 first entry is not meta.nup");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    for (std::size_t index = 1; index < entries.size(); ++index) {
        if (!IsValidPath(entries[index].name)) {
            LOG_ERROR(Loader, "NUP2 entry {} has an invalid path", index);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
    }
    return true;
}

bool Nup2Container::ReadMetaEntryBytes(std::vector<u8>& out) const {
    const Entry& meta = entries[0];
    if (meta.size > MaxMetaRecordSize || meta.size > file_size) {
        LOG_ERROR(Loader, "NUP2 meta record claims an implausible {} byte size", meta.size);
        return false;
    }
    out = file->ReadBytes(static_cast<std::size_t>(meta.size),
                          static_cast<std::size_t>(data_start + meta.data_offset));
    return out.size() == meta.size;
}

bool Nup2Container::ParseTlvs(PackageInfo& package, const std::vector<u8>& block) {
    std::map<u16, std::vector<u8>> tlvs;
    std::size_t pos = 0;
    while (pos < block.size()) {
        if (pos + 4 > block.size()) {
            LOG_ERROR(Loader, "NUP2 TLV header overruns the TLV block");
            return false;
        }
        const u16 id = ReadU16(block.data() + pos);
        const u16 length = ReadU16(block.data() + pos + 2);
        pos += 4;
        if (pos + length > block.size()) {
            LOG_ERROR(Loader, "NUP2 TLV value overruns the TLV block");
            return false;
        }
        tlvs.emplace(id, std::vector<u8>(block.begin() + pos, block.begin() + pos + length));
        pos += length;
    }
    const auto text = [&](TlvId id) -> std::string {
        const auto it = tlvs.find(static_cast<u16>(id));
        return it == tlvs.end() ? std::string{} : std::string(it->second.begin(), it->second.end());
    };
    package.display_version = text(TlvDisplayVersion);
    package.title_name = text(TlvTitleName);
    package.publisher = text(TlvPublisher);
    package.build_id = text(TlvBuildId);
    package.source_dump = text(TlvSourceDump);
    if (const auto it = tlvs.find(static_cast<u16>(TlvIcon)); it != tlvs.end()) {
        package.icon = it->second;
    }
    const auto parse_localized = [&](TlvId id, auto& out) -> bool {
        const auto it = tlvs.find(static_cast<u16>(id));
        if (it == tlvs.end()) {
            return true;
        }
        const std::vector<u8>& data = it->second;
        std::size_t pos = 0;
        while (pos < data.size()) {
            const u8 lang_length = data[pos];
            pos += 1;
            if (pos + lang_length + 2 > data.size()) {
                LOG_ERROR(Loader, "NUP2 localized record overruns the TLV");
                return false;
            }
            std::string lang(reinterpret_cast<const char*>(data.data()) + pos, lang_length);
            pos += lang_length;
            if (pos + 2 > data.size()) {
                LOG_ERROR(Loader, "NUP2 localized record overruns the TLV");
                return false;
            }
            const u16 value_length = ReadU16(data.data() + pos);
            pos += 2;
            if (pos + value_length > data.size()) {
                LOG_ERROR(Loader, "NUP2 localized record value overruns the TLV");
                return false;
            }
            using OutEntry = typename std::decay_t<decltype(out)>::value_type;
            if constexpr (std::is_same_v<OutEntry, std::pair<std::string, std::string>>) {
                out.emplace_back(std::move(lang),
                                 std::string(data.begin() + pos, data.begin() + pos + value_length));
            } else {
                out.emplace_back(std::move(lang),
                                 std::vector<u8>(data.begin() + pos, data.begin() + pos + value_length));
            }
            pos += value_length;
        }
        return true;
    };
    if (!parse_localized(TlvLocalizedTitle, package.localized_titles)) {
        return false;
    }
    if (!parse_localized(TlvLocalizedIcon, package.localized_icons)) {
        return false;
    }
    return true;
}

bool Nup2Container::ParseFileRows(PackageInfo& package, const u8* data, std::size_t size,
                                  std::size_t& pos) const {
    for (auto& row : package.files) {
        const void* nul = std::memchr(data + pos, '\0', size - pos);
        if (nul == nullptr) {
            LOG_ERROR(Loader, "NUP2 file table path is not NUL-terminated");
            return false;
        }
        const auto length = static_cast<std::size_t>(static_cast<const u8*>(nul) - (data + pos));
        row.path.assign(reinterpret_cast<const char*>(data + pos), length);
        pos += length + 1;
        if (pos + 8 + DigestSize > size) {
            LOG_ERROR(Loader, "NUP2 file table row overruns the record");
            return false;
        }
        row.size = ReadU64(data + pos);
        std::memcpy(row.sha256.data(), data + pos + 8, DigestSize);
        pos += 8 + DigestSize;
        if (!IsValidPath(row.path)) {
            LOG_ERROR(Loader, "NUP2 file table row {} has an invalid path", row.path);
            return false;
        }
    }
    return true;
}

bool Nup2Container::ParseMeta() {
    std::vector<u8> meta;
    if (!ReadMetaEntryBytes(meta)) {
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    if (meta.size() < MetaHeaderSize + DigestSize) {
        LOG_ERROR(Loader, "NUP2 meta record is too short");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const std::size_t body_size = meta.size() - DigestSize;
    std::array<u8, DigestSize> stored_digest{};
    std::memcpy(stored_digest.data(), meta.data() + body_size, DigestSize);
    if (Sha256(meta.data(), body_size) != stored_digest) {
        LOG_ERROR(Loader, "NUP2 meta record digest mismatch");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const auto prefix_region = file->ReadBytes(data_start, 0);
    if (prefix_region.size() != data_start) {
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    std::array<u8, DigestSize> stored_header_hash{};
    std::memcpy(stored_header_hash.data(), meta.data() + 0x18, DigestSize);
    if (Sha256(prefix_region.data(), prefix_region.size()) != stored_header_hash) {
        LOG_ERROR(Loader, "NUP2 container header hash mismatch");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    if (ReadU32(meta.data() + 12) != 0 || ReadU32(meta.data() + 0x38) != 0 ||
        ReadU32(meta.data() + 0x3C) != 0) {
        LOG_ERROR(Loader, "NUP2 meta reserved or feature-flag fields are not zero");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const u16 meta_header_size = ReadU16(meta.data() + 4);
    if (meta_header_size < MetaHeaderSize || meta_header_size > body_size) {
        LOG_ERROR(Loader, "NUP2 meta header size is implausible");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    const u32 package_count = ReadU32(meta.data() + 8);
    const u64 declared_file_count = ReadU64(meta.data() + 0x10);
    std::size_t pos = meta_header_size;
    u64 expected_first_index = 1;
    packages.resize(package_count);
    for (u32 record_index = 0; record_index < package_count; ++record_index) {
        if (pos + PackageFixedSize > body_size) {
            LOG_ERROR(Loader, "NUP2 package record overruns the meta record");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        const u8* record = meta.data() + pos;
        pos += PackageFixedSize;
        auto& package = packages[record_index];
        const u8 kind_value = record[0x00];
        package.flags = record[0x01];
        if (kind_value > static_cast<u8>(PackageKind::Addon)) {
            LOG_ERROR(Loader, "NUP2 package has unknown kind {}", kind_value);
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        package.kind = static_cast<PackageKind>(kind_value);
        if ((package.kind == PackageKind::Update) != ((package.flags & FlagDiff) != 0)) {
            LOG_ERROR(Loader, "NUP2 package kind/flags combination is invalid");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        package.title_id = ReadU64(record + 0x04);
        package.application_id = ReadU64(record + 0x0C);
        if (package.kind != PackageKind::Base && package.application_id == 0) {
            LOG_ERROR(Loader, "NUP2 non-base package has no application id");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        package.version = ReadU32(record + 0x14);
        const u32 file_count = ReadU32(record + 0x18);
        std::memcpy(package.content_hash.data(), record + 0x1C, 0x20);
        std::memcpy(package.base_content_hash.data(), record + 0x3C, 0x20);
        package.first_entry_index = ReadU64(record + 0x5C);
        if (ReadU32(record + 0x64) != 0) {
            LOG_ERROR(Loader, "NUP2 package reserved field is not zero");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        const u16 tlv_size = ReadU16(record + 0x02);
        if (pos + tlv_size > body_size) {
            LOG_ERROR(Loader, "NUP2 TLV block overruns the meta record");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        const auto tlv_block =
            std::vector<u8>(meta.begin() + static_cast<std::ptrdiff_t>(pos),
                            meta.begin() + static_cast<std::ptrdiff_t>(pos + tlv_size));
        if (!ParseTlvs(package, tlv_block)) {
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        pos += tlv_size;
        package.files.resize(file_count);
        if (!ParseFileRows(package, meta.data(), body_size, pos)) {
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        if (package.first_entry_index != expected_first_index) {
            LOG_ERROR(Loader, "NUP2 package scopes are not contiguous");
            status = Loader::ResultStatus::ErrorNUPBadFormat;
            return false;
        }
        expected_first_index += file_count;
    }
    if (pos != body_size) {
        LOG_ERROR(Loader, "NUP2 has {} trailing bytes after the last package record",
                  body_size - pos);
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    if (expected_first_index - 1 != declared_file_count ||
        expected_first_index != entry_count) {
        LOG_ERROR(Loader, "NUP2 meta file count disagrees with the container");
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    if (!VerifyPinning()) {
        status = Loader::ResultStatus::ErrorNUPBadFormat;
        return false;
    }
    return true;
}

bool Nup2Container::VerifyPinning() const {
    for (const auto& package : packages) {
        const std::string prefix = ScopePrefix(package);
        for (std::size_t offset = 0; offset < package.files.size(); ++offset) {
            const auto& row = package.files[offset];
            const Entry& entry = entries[package.first_entry_index + offset];
            const std::string expected = prefix + "/" + row.path;
            if (entry.name != expected) {
                LOG_ERROR(Loader,
                          "NUP2 entry {} does not match its file table row",
                          package.first_entry_index + offset);
                return false;
            }
            if (entry.size != row.size) {
                LOG_ERROR(Loader, "NUP2 entry size disagrees with the file table");
                return false;
            }
        }
    }
    return true;
}

std::string Nup2Container::ScopePrefix(const PackageInfo& package) const {
    switch (package.kind) {
    case PackageKind::Base:
        return "base";
    case PackageKind::Update:
        return fmt::format("update/{}", package.version);
    case PackageKind::Addon:
    default:
        return fmt::format("addon/{}", package.title_id - package.application_id - 0x1000);
    }
}

std::optional<std::size_t> Nup2Container::FindBasePackage() const {
    for (std::size_t index = 0; index < packages.size(); ++index) {
        if (packages[index].kind == PackageKind::Base) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Nup2Container::FindBestUpdatePackage() const {
    std::optional<std::size_t> best;
    for (std::size_t index = 0; index < packages.size(); ++index) {
        if (packages[index].kind != PackageKind::Update) {
            continue;
        }
        if (!best || packages[index].version > packages[*best].version) {
            best = index;
        }
    }
    return best;
}

VirtualFile Nup2Container::OpenEntry(std::size_t entry_index) {
    if (entry_index >= entries.size()) {
        return nullptr;
    }
    const Entry& entry = entries[entry_index];
    return std::make_shared<OffsetVfsFile>(
        file, static_cast<std::size_t>(entry.size),
        static_cast<std::size_t>(data_start + entry.data_offset), entry.name);
}

VirtualDir Nup2Container::GetPackageDir(std::size_t package_index) {
    if (package_index >= packages.size()) {
        return nullptr;
    }
    if (scope_dir_cache[package_index] != nullptr) {
        return scope_dir_cache[package_index];
    }
    const auto& package = packages[package_index];
    const std::string prefix = ScopePrefix(package);
    auto root = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                     std::vector<VirtualDir>{}, prefix);
    // VectorVfsDirectory::CreateSubdirectory is a stub, so the scope tree is
    // built by hand: one VectorVfsDirectory per path level, parents first.
    std::map<std::string, VectorVfsDirectory*> dirs;
    dirs[""] = root.get();
    for (std::size_t offset = 0; offset < package.files.size(); ++offset) {
        const auto& row = package.files[offset];
        std::size_t slash = row.path.find('/');
        while (slash != std::string::npos) {
            const std::string dir_path = row.path.substr(0, slash);
            if (!dirs.contains(dir_path)) {
                const auto parent_slash = dir_path.rfind('/');
                const std::string parent =
                    parent_slash == std::string::npos ? std::string{}
                                                      : dir_path.substr(0, parent_slash);
                const std::string name = dir_path.substr(parent_slash + 1);
                auto created = std::make_shared<VectorVfsDirectory>(
                    std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, name);
                dirs[parent]->AddDirectory(created);
                dirs[dir_path] = created.get();
            }
            slash = row.path.find('/', slash + 1);
        }
        const auto parent_slash = row.path.rfind('/');
        const std::string parent_dir =
            parent_slash == std::string::npos ? std::string{} : row.path.substr(0, parent_slash);
        const auto name_slash = row.path.rfind('/');
        const std::string file_name =
            name_slash == std::string::npos ? row.path : row.path.substr(name_slash + 1);
        dirs[parent_dir]->AddFile(std::make_shared<OffsetVfsFile>(
            file, static_cast<std::size_t>(row.size),
            static_cast<std::size_t>(data_start +
                                     entries[package.first_entry_index + offset].data_offset),
            file_name));
    }
    scope_dir_cache[package_index] = root;
    return root;
}

VirtualDir Nup2Container::GetExeFSDir(std::size_t package_index) {
    auto dir = GetPackageDir(package_index);
    return dir == nullptr ? nullptr : dir->GetSubdirectory("exefs");
}

VirtualDir Nup2Container::GetRomFSDir(std::size_t package_index) {
    auto dir = GetPackageDir(package_index);
    return dir == nullptr ? nullptr : dir->GetSubdirectory("romfs");
}

Loader::ResultStatus Nup2Container::VerifyIntegrity(
    std::function<bool(std::size_t, std::size_t)> progress_callback) const {
    if (status != Loader::ResultStatus::Success) {
        return status;
    }
    const std::size_t total = entries.size() - 1;
    std::size_t done = 0;
    for (const auto& package : packages) {
        for (std::size_t offset = 0; offset < package.files.size(); ++offset) {
            const auto& row = package.files[offset];
            const Entry& entry = entries[package.first_entry_index + offset];
            const auto data = file->ReadBytes(static_cast<std::size_t>(entry.size),
                                              static_cast<std::size_t>(data_start + entry.data_offset));
            if (data.size() != entry.size) {
                LOG_ERROR(Loader, "NUP2 entry ended early");
                return Loader::ResultStatus::ErrorNUPBadFormat;
            }
            if (Sha256Span(data) != row.sha256) {
                LOG_ERROR(Loader, "NUP2 entry data hash mismatch");
                return Loader::ResultStatus::ErrorNUPBadFormat;
            }
            if (entry.hashed_size > 0 && entry.hashed_size <= entry.size &&
                Sha256(data.data(), entry.hashed_size) != entry.prefix_hash) {
                LOG_ERROR(Loader, "NUP2 entry prefix hash mismatch");
                return Loader::ResultStatus::ErrorNUPBadFormat;
            }
            ++done;
            if (progress_callback && !progress_callback(done, total)) {
                return Loader::ResultStatus::ErrorNUPBadFormat;
            }
        }
    }
    return Loader::ResultStatus::Success;
}

} // namespace FileSys
