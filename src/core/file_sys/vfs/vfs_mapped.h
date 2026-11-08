// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

/// A read-only file that is memory-mapped whole. Reads are a memcpy out of the mapping (the system faults pages in
/// on demand, so there is no seek, no buffered-stdio copy and no lock).
/// The mapping is file-backed, so it adds no commit charge.
///
/// A read of a part of the file the disk cannot deliver raises an in-page error instead of returning a short
/// read, so this is meant for files on a healthy drive.
class MappedVfsFile final : public VfsFile {
public:
    /// nullptr if the file cannot be opened or mapped.
    static std::shared_ptr<MappedVfsFile> Open(const std::string& path);

    ~MappedVfsFile() override;

    std::string GetName() const override;
    std::size_t GetSize() const override;
    bool Resize(std::size_t new_size) override;
    VirtualDir GetContainingDirectory() const override;
    bool IsWritable() const override;
    bool IsReadable() const override;
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override;
    std::size_t Write(const u8* data, std::size_t length, std::size_t offset) override;
    bool Rename(std::string_view new_name) override;

    /// Reads shorter than this come straight out of the mapping; longer reads use a positional read.
    static constexpr std::size_t MemcpyReadLimit = 256 * 1024;

private:
    MappedVfsFile() = default;

    std::string path;
    const u8* data{};
    std::size_t size{};
#ifdef _WIN32
    void* file_handle{};
    void* mapping_handle{};
#else
    int fd{-1};
#endif
};

} // namespace FileSys
