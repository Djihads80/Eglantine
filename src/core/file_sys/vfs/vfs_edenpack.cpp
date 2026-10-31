// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs_edenpack.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {
namespace {

constexpr std::array<u8, 8> Magic{'E', 'D', 'E', 'N', 'P', 'K', '0', '1'};
constexpr std::size_t HeaderSize = 40;
constexpr std::size_t MaxIndexSize = 64 * 1024 * 1024;
constexpr u32 FormatVersion = 1;

u32 ReadU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u64 ReadU64(const u8* p) {
    u64 value{};
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<u64>(p[i]) << (i * 8);
    }
    return value;
}

bool SafePath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != path.npos ||
        path.find('\0') != path.npos) {
        return false;
    }
    std::size_t start{};
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end == path.npos ? path.size() : end + 1;
    }
    return path.starts_with("ExeFS/") || path.starts_with("RomFS/") ||
           path == "ExeFS" || path == "RomFS";
}

class EdenPackFile final : public VfsFile {
public:
    EdenPackFile(VirtualFile archive_, std::string name_, u64 offset_, u64 size_,
                 VirtualDir parent_)
        : archive(std::move(archive_)), name(std::move(name_)), offset(offset_), size(size_),
          parent(std::move(parent_)) {}

    std::string GetName() const override { return name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(size); }
    bool Resize(std::size_t) override { return false; }
    VirtualDir GetContainingDirectory() const override { return parent; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* data, std::size_t length, std::size_t position) const override {
        if (length == 0 || static_cast<u64>(position) >= size) {
            return 0;
        }
        if (!data || !archive) {
            LOG_ERROR(Loader, "EdenPack read rejected for {}: null buffer or archive", name);
            return 0;
        }

        const u64 relative_position = static_cast<u64>(position);
        if (relative_position > std::numeric_limits<u64>::max() - offset) {
            LOG_ERROR(Loader, "EdenPack offset overflow for {}: base={}, position={}", name,
                      offset, relative_position);
            return 0;
        }

        const u64 archive_position = offset + relative_position;
        const u64 requested = std::min<u64>(static_cast<u64>(length), size - relative_position);
        if (archive_position > std::numeric_limits<std::size_t>::max() ||
            requested > std::numeric_limits<std::size_t>::max()) {
            LOG_ERROR(Loader, "EdenPack read exceeds addressable range for {}: offset={}, size={}",
                      name, archive_position, requested);
            return 0;
        }

        const auto count = static_cast<std::size_t>(requested);
        const auto actual = archive->Read(data, count, static_cast<std::size_t>(archive_position));
        if (actual != count) {
            LOG_WARNING(Loader,
                        "EdenPack short read: file={}, archive_offset={}, file_position={}, "
                        "requested={}, actual={}",
                        name, archive_position, relative_position, count, actual);
        }
        return actual;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    VirtualFile archive;
    std::string name;
    u64 offset;
    u64 size;
    VirtualDir parent;
};

struct PendingFile {
    std::string path;
    u64 offset{};
    u64 size{};
};

bool BuildDirectoryTree(const VirtualFile& archive, const std::vector<PendingFile>& files,
                        const std::vector<std::string>& directories,
                        const std::shared_ptr<VectorVfsDirectory>& root) {
    std::map<std::string, std::shared_ptr<VectorVfsDirectory>, std::less<>> nodes;
    nodes.emplace("", root);

    auto ensure_directory = [&](std::string_view path) -> std::shared_ptr<VectorVfsDirectory> {
        std::string current;
        auto parent = root;
        std::size_t start{};
        while (start < path.size()) {
            const auto end = path.find('/', start);
            const std::string part(path.substr(start, end == path.npos ? path.size() - start : end - start));
            current = current.empty() ? part : current + "/" + part;
            auto it = nodes.find(current);
            if (it == nodes.end()) {
                auto child = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                                  std::vector<VirtualDir>{},
                                                                  part, parent);
                parent->AddDirectory(child);
                nodes.emplace(current, child);
                parent = std::move(child);
            } else {
                parent = it->second;
            }
            if (end == path.npos) break;
            start = end + 1;
        }
        return parent;
    };

    for (const auto& path : directories) {
        if (path != "ExeFS" && path != "RomFS") {
            const auto slash = path.find_last_of('/');
            ensure_directory(path.substr(0, slash));
        } else {
            ensure_directory(path);
        }
    }
    for (const auto& file : files) {
        const auto slash = file.path.find_last_of('/');
        const std::string parent_path = slash == std::string::npos ? "" : file.path.substr(0, slash);
        const std::string name = slash == std::string::npos ? file.path : file.path.substr(slash + 1);
        const auto parent = ensure_directory(parent_path);
        parent->AddFile(std::make_shared<EdenPackFile>(archive, name, file.offset, file.size, parent));
    }
    return true;
}

} // namespace

EdenPackVfsFilesystem::EdenPackVfsFilesystem(VirtualFile archive)
    : VfsFilesystem(nullptr), archive(std::move(archive)) {
    if (!this->archive || this->archive->GetSize() < HeaderSize) return;

    std::array<u8, HeaderSize> header{};
    if (this->archive->Read(header.data(), header.size(), 0) != header.size() ||
        !std::equal(Magic.begin(), Magic.end(), header.begin())) {
        return;
    }

    const u32 version = ReadU32(header.data() + 8);
    const u32 flags = ReadU32(header.data() + 12);
    const u64 index_offset = ReadU64(header.data() + 16);
    const u64 index_size = ReadU64(header.data() + 24);
    const u64 entry_count = ReadU64(header.data() + 32);
    const u64 archive_size = this->archive->GetSize();
    if (version != FormatVersion || flags != 0 || index_size == 0 ||
        index_size > MaxIndexSize || index_offset < HeaderSize ||
        index_offset > archive_size || index_size > archive_size - index_offset ||
        entry_count > 1'000'000) {
        return;
    }

    std::vector<u8> index_bytes(static_cast<std::size_t>(index_size));
    if (this->archive->Read(index_bytes.data(), index_bytes.size(),
                            static_cast<std::size_t>(index_offset)) != index_bytes.size()) {
        return;
    }

    try {
        const auto json = nlohmann::json::parse(index_bytes.begin(), index_bytes.end());
        if (json.at("format") != "EdenPack" || json.at("version").get<u32>() != FormatVersion) {
            return;
        }
        const auto& entries = json.at("entries");
        if (!entries.is_array() || entries.size() != entry_count) return;

        std::vector<PendingFile> files;
        std::vector<std::string> directories;
        std::vector<std::pair<u64, u64>> data_ranges;
        std::map<std::string, std::string, std::less<>> seen_paths;
        files.reserve(entries.size());
        directories.reserve(entries.size());
        data_ranges.reserve(entries.size());
        for (const auto& entry : entries) {
            const auto path = entry.at("path").get<std::string>();
            if (!SafePath(path)) return;
            const auto type = entry.at("type").get<std::string>();
            if (type != "directory" && type != "file") return;
            if (!seen_paths.emplace(path, type).second) {
                LOG_ERROR(Loader, "Duplicate EdenPack path: {}", path);
                return;
            }

            if (type == "directory") {
                directories.push_back(path);
            } else {
                const u64 offset = entry.at("offset").get<u64>();
                const u64 size = entry.at("size").get<u64>();
                if (offset < HeaderSize || offset > index_offset || size > index_offset - offset) {
                    LOG_ERROR(Loader, "EdenPack file range is outside the data area: {}", path);
                    return;
                }
                if (size != 0) {
                    data_ranges.emplace_back(offset, offset + size);
                }
                files.push_back({path, offset, size});
            }
        }

        std::sort(data_ranges.begin(), data_ranges.end());
        for (std::size_t i = 1; i < data_ranges.size(); ++i) {
            if (data_ranges[i].first < data_ranges[i - 1].second) {
                LOG_ERROR(Loader,
                          "Overlapping EdenPack file data ranges: [{}, {}) and [{}, {})",
                          data_ranges[i - 1].first, data_ranges[i - 1].second,
                          data_ranges[i].first, data_ranges[i].second);
                return;
            }
        }

        auto root = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                          std::vector<VirtualDir>{},
                                                          "EdenPack");
        if (!BuildDirectoryTree(this->archive, files, directories, root)) return;
        this->root = std::move(root);
    } catch (const std::exception& e) {
        LOG_ERROR(Loader, "Invalid EdenPack index: {}", e.what());
    }
}

VirtualDir EdenPackVfsFilesystem::Open(VirtualFile archive) {
    auto filesystem = std::make_shared<EdenPackVfsFilesystem>(std::move(archive));
    if (!filesystem->root) return nullptr;
    return filesystem->root;
}

std::string EdenPackVfsFilesystem::GetName() const { return "EdenPack"; }
bool EdenPackVfsFilesystem::IsWritable() const { return false; }
bool EdenPackVfsFilesystem::IsReadable() const { return root != nullptr; }

} // namespace FileSys
