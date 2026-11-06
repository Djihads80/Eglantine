// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/swap.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fsmitm_romfsbuild.h"
#include "core/file_sys/nx2_package.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

namespace {

using namespace Common::Literals;

// The PFS0 layout PartitionFilesystem reads: header, entries, string table, then the data, with
// entry offsets relative to the end of the string table.
struct PfsHeader {
    u32_le magic;
    u32_le num_entries;
    u32_le strtab_size;
    u32_le reserved;
};
static_assert(sizeof(PfsHeader) == 0x10);

struct PfsEntry {
    u64_le offset;
    u64_le size;
    u32_le strtab_offset;
    u32_le reserved;
};
static_assert(sizeof(PfsEntry) == 0x18);

constexpr size_t CopyChunkSize = 16_MiB;

// A file of the package: one source file, or the RomFS image as its parts (offset in the image,
// file); the gaps between parts are zero.
struct PackageEntry {
    std::string name;
    u64 size;
    std::vector<std::pair<u64, VirtualFile>> parts;
};

struct CopyJob {
    u64 destination;
    u64 size;
    VirtualFile file;
};

bool IsSidecar(const VirtualFile& file) {
    const auto extension = file->GetExtension();
    return extension == "nacp" || extension == "png" || extension == "jpg" ||
           extension == "jpeg" || extension == "bmp" ||
           (extension == "dat" && file->GetName().starts_with("icon_"));
}

} // Anonymous namespace

std::string PackNx2(const VirtualDir& dump_dir, const std::string& out_path,
                    const Nx2PackProgress& progress) {
    if (dump_dir == nullptr) {
        return "The dump folder could not be opened.";
    }
    const VirtualDir exefs =
        IsDirectoryExeFS(dump_dir) ? dump_dir : dump_dir->GetSubdirectory("exefs");
    if (exefs == nullptr || !IsDirectoryExeFS(exefs)) {
        return "No ExeFS found: the folder needs main and main.npdm, directly or in an exefs "
               "folder.";
    }
    const VirtualDir title_dir = exefs == dump_dir ? exefs->GetParentDirectory() : dump_dir;

    std::vector<PackageEntry> entries;
    const auto add = [&entries](std::string name, VirtualFile file) {
        const bool present = std::any_of(entries.begin(), entries.end(),
                                         [&name](const auto& entry) { return entry.name == name; });
        if (!present) {
            const u64 size = file->GetSize();
            entries.push_back({std::move(name), size, {{0, std::move(file)}}});
        }
    };

    // The ExeFS (main, main.npdm, rtld, sdk, subsdk*, sidecars dropped next to them). A RomFS
    // image already next to main is taken as the RomFS.
    VirtualFile romfs_image;
    for (const auto& file : exefs->GetFiles()) {
        if (file->GetExtension() == "romfs") {
            if (romfs_image == nullptr) {
                romfs_image = file;
            }
            continue;
        }
        add(file->GetName(), file);
    }

    // Sidecars in the game folder: a PFS0 has no parent for the loader to look in.
    if (title_dir != nullptr && title_dir != exefs) {
        for (const auto& file : title_dir->GetFiles()) {
            if (IsSidecar(file)) {
                add(file->GetName(), file);
            }
        }
    }

    // The RomFS: that image, else the romfs/ folder next to the ExeFS laid out as one image.
    if (romfs_image != nullptr) {
        add(std::string{Nx2RomFSName}, romfs_image);
    } else if (const auto romfs_dir =
                   title_dir != nullptr ? title_dir->GetSubdirectory("romfs") : nullptr) {
        auto parts = RomFSBuildContext{romfs_dir}.Build();
        if (!parts.empty()) {
            const u64 size = parts.back().first + parts.back().second->GetSize();
            entries.push_back({std::string{Nx2RomFSName}, size, std::move(parts)});
        }
    } else {
        LOG_WARNING(Loader, "Packing {} without a RomFS", out_path);
    }

    // String table, padded so the data starts 0x20-aligned like Nintendo's own PFS0s.
    std::vector<char> strtab;
    std::vector<PfsEntry> pfs_entries;
    u64 data_size = 0;
    for (const auto& entry : entries) {
        pfs_entries.push_back(PfsEntry{
            .offset = data_size,
            .size = entry.size,
            .strtab_offset = static_cast<u32>(strtab.size()),
            .reserved = 0,
        });
        strtab.insert(strtab.end(), entry.name.begin(), entry.name.end());
        strtab.push_back('\0');
        data_size = Common::AlignUp(data_size + entry.size, 0x10);
    }
    const size_t metadata_size =
        sizeof(PfsHeader) + pfs_entries.size() * sizeof(PfsEntry) + strtab.size();
    strtab.resize(strtab.size() + Common::AlignUp(metadata_size, 0x20) - metadata_size, '\0');
    const u64 data_start = Common::AlignUp(metadata_size, 0x20);

    const PfsHeader header{
        .magic = Common::MakeMagic('P', 'F', 'S', '0'),
        .num_entries = static_cast<u32>(pfs_entries.size()),
        .strtab_size = static_cast<u32>(strtab.size()),
        .reserved = 0,
    };

    std::vector<CopyJob> jobs;
    u64 total = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        for (const auto& [offset, file] : entries[i].parts) {
            const u64 size = file->GetSize();
            if (size != 0) {
                jobs.push_back({data_start + pfs_entries[i].offset + offset, size, file});
                total += size;
            }
        }
    }

    Common::FS::IOFile out{out_path, Common::FS::FileAccessMode::Write,
                           Common::FS::FileType::BinaryFile};
    if (!out.IsOpen()) {
        return "The output file could not be created.";
    }
    const auto fail = [&out, &out_path](std::string error) {
        out.Close();
        Common::FS::RemoveFile(out_path);
        return error;
    };

    // Not sized up front: on Windows that writes every byte as zero first. The jobs are taken in
    // offset order, so the file grows as they land; the OS zero-fills the gaps and paddings.
    if (!out.WriteObject(header) ||
        out.WriteSpan(std::span<const PfsEntry>{pfs_entries}) != pfs_entries.size() ||
        out.WriteSpan(std::span<const char>{strtab}) != strtab.size()) {
        return fail("Writing the package failed (is the disk full?).");
    }
    if (progress && !progress(0, total)) {
        return fail("Cancelled.");
    }

    // A RomFS is mostly small files, and copying them one by one waits on each file's open
    // (antivirus scans included) with the disk idle. Several readers keep it busy; real files
    // are read through their own handles, since the VFS serialises its reads behind one lock.
    std::atomic<size_t> next_job{0};
    std::atomic<u64> copied{0};
    std::atomic<bool> stop{false};
    std::atomic<size_t> running{0};
    std::mutex out_mutex;
    std::mutex error_mutex;
    std::string copy_error;
    const auto set_error = [&](std::string error) {
        std::scoped_lock lk{error_mutex};
        if (copy_error.empty()) {
            copy_error = std::move(error);
        }
        stop = true;
    };

    const auto copy_worker = [&] {
        std::vector<u8> buffer(CopyChunkSize);
        while (!stop) {
            const size_t index = next_job.fetch_add(1);
            if (index >= jobs.size()) {
                break;
            }
            const CopyJob& job = jobs[index];

            Common::FS::IOFile direct;
            const auto path = job.file->GetFullPath();
            if (Common::FS::IsFile(path)) {
                direct.Open(std::filesystem::path{Common::FS::ToU8String(path)},
                            Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile);
                if (direct.IsOpen() && direct.GetSize() != job.size) {
                    direct.Close();
                }
            }

            for (u64 position = 0; position < job.size && !stop;) {
                const size_t length =
                    static_cast<size_t>((std::min<u64>)(CopyChunkSize, job.size - position));
                const auto data = std::span<u8>{buffer.data(), length};
                const size_t read = direct.IsOpen() ? direct.ReadSpan(data)
                                                    : job.file->Read(data.data(), length, position);
                if (read != length) {
                    set_error("Reading " + path + " failed.");
                    break;
                }
                {
                    std::scoped_lock lk{out_mutex};
                    if (!out.Seek(static_cast<s64>(job.destination + position)) ||
                        out.WriteSpan(std::span<const u8>{data}) != length) {
                        set_error("Writing the package failed (is the disk full?).");
                        break;
                    }
                }
                position += length;
                copied += length;
            }
        }
        --running;
    };

    const size_t reader_count =
        std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 4, 16);
    std::vector<std::thread> readers;
    running = reader_count;
    for (size_t i = 0; i < reader_count; ++i) {
        readers.emplace_back(copy_worker);
    }
    bool cancelled = false;
    while (running > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        if (progress && !cancelled && !progress(copied, total)) {
            cancelled = true;
            stop = true;
        }
    }
    for (auto& reader : readers) {
        reader.join();
    }

    if (cancelled) {
        return fail("Cancelled.");
    }
    if (!copy_error.empty()) {
        return fail(copy_error);
    }
    // The padding after the last file
    if (!out.SetSize(data_start + data_size) || !out.Flush()) {
        return fail("Writing the package failed (is the disk full?).");
    }
    if (progress) {
        progress(total, total);
    }
    LOG_INFO(Loader, "Packed {} files ({} bytes) into {}", jobs.size(), data_start + data_size,
             out_path);
    return {};
}

} // namespace FileSys
