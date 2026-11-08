// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/vfs/vfs_mapped.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "common/logging.h"

namespace FileSys {

std::shared_ptr<MappedVfsFile> MappedVfsFile::Open(const std::string& path) {
    std::shared_ptr<MappedVfsFile> file{new MappedVfsFile};
    file->path = path;
    const std::filesystem::path native{std::filesystem::u8path(path)};
#ifdef _WIN32
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length) || length.QuadPart <= 0) {
        CloseHandle(handle);
        return nullptr;
    }
    HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(handle);
        return nullptr;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(handle);
        return nullptr;
    }
    file->file_handle = handle;
    file->mapping_handle = mapping;
    // (the handle keeps FILE_FLAG_RANDOM_ACCESS: the game's reads jump around)
    file->data = static_cast<const u8*>(view);
    file->size = static_cast<std::size_t>(length.QuadPart);
#else
    const int fd = ::open(native.c_str(), O_RDONLY);
    if (fd < 0) {
        return nullptr;
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        ::close(fd);
        return nullptr;
    }
    void* view = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        return nullptr;
    }
    file->fd = fd;
    file->data = static_cast<const u8*>(view);
    file->size = static_cast<std::size_t>(info.st_size);
#endif
    LOG_INFO(Common_Filesystem, "Mapped {} ({} MiB)", path, file->size >> 20);
    return file;
}

MappedVfsFile::~MappedVfsFile() {
#ifdef _WIN32
    if (data != nullptr) {
        UnmapViewOfFile(data);
    }
    if (mapping_handle != nullptr) {
        CloseHandle(mapping_handle);
    }
    if (file_handle != nullptr) {
        CloseHandle(file_handle);
    }
#else
    if (data != nullptr) {
        munmap(const_cast<u8*>(data), size);
    }
    if (fd >= 0) {
        ::close(fd);
    }
#endif
}

std::string MappedVfsFile::GetName() const {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::size_t MappedVfsFile::GetSize() const {
    return size;
}

bool MappedVfsFile::Resize(std::size_t) {
    return false;
}

VirtualDir MappedVfsFile::GetContainingDirectory() const {
    return nullptr;
}

bool MappedVfsFile::IsWritable() const {
    return false;
}

bool MappedVfsFile::IsReadable() const {
    return true;
}

std::size_t MappedVfsFile::Read(u8* destination, std::size_t length, std::size_t offset) const {
    if (offset >= size) {
        return 0;
    }
    length = std::min(length, size - offset);
    if (length < MemcpyReadLimit) {
        std::memcpy(destination, data + offset, length);
        return length;
    }
    // Large reads: copying out of a fresh mapping faults every page in one at a time and measured slower than a
    // positional read, which the system serves with large copies. Neither takes a lock.
    std::size_t done_total = 0;
    while (done_total < length) {
#ifdef _WIN32
        OVERLAPPED position{};
        const u64 at = offset + done_total;
        position.Offset = static_cast<DWORD>(at & 0xffffffffu);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD done = 0;
        if (!::ReadFile(file_handle, destination + done_total,
                        static_cast<DWORD>(std::min<std::size_t>(length - done_total, 1u << 30)), &done, &position) ||
            done == 0) {
            break;
        }
#else
        const ssize_t done = pread(fd, destination + done_total, length - done_total,
                                   static_cast<off_t>(offset + done_total));
        if (done <= 0) {
            break;
        }
#endif
        done_total += static_cast<std::size_t>(done);
    }
    return done_total;
}

std::size_t MappedVfsFile::Write(const u8*, std::size_t, std::size_t) {
    return 0;
}

bool MappedVfsFile::Rename(std::string_view) {
    return false;
}

} // namespace FileSys
